pub mod acpi;
pub mod config;
pub mod discover;
pub mod dtb;
pub mod iommu;
pub mod quirks;
pub mod serve;

use std::sync::atomic::{AtomicBool, AtomicI32, AtomicI64, AtomicU8, AtomicU32, Ordering};
use std::sync::{Mutex, OnceLock};

use anyhow::Result;
use arch::{IoMemSpace, bit_register, scalar_register};

use config::PciConfigIo;

pub(crate) use crate::irq::{IrqPin, system_irq};
pub(crate) use crate::leak;

// The PCI tree is only locked for the duration of a single operation, none of which can panic.
pub(crate) const EXPECT_LOCK: &str = "sif: PCI tree mutex was poisoned";

// thor only implements MSI allocation on x86-64 (LAPIC MSIs); mirror that here
// until the kernel can report MSI availability.
pub fn msi_controller_available() -> bool {
    cfg!(target_arch = "x86_64")
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum IrqIndex {
    Null = 0,
    IntA = 1,
    IntB = 2,
    IntC = 3,
    IntD = 4,
}

impl IrqIndex {
    pub fn from_pin(pin: u8) -> IrqIndex {
        match pin {
            1 => IrqIndex::IntA,
            2 => IrqIndex::IntB,
            3 => IrqIndex::IntC,
            4 => IrqIndex::IntD,
            _ => IrqIndex::Null,
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            IrqIndex::IntA => "INTA",
            IrqIndex::IntB => "INTB",
            IrqIndex::IntC => "INTC",
            IrqIndex::IntD => "INTD",
            IrqIndex::Null => panic!("Illegal PCI interrupt pin"),
        }
    }
}

pub fn name_of_capability(type_: u32) -> Option<&'static str> {
    match type_ {
        0x01 => Some("PCI Power Management Interface"),
        0x02 => Some("AGP"),
        0x03 => Some("VPD"),
        0x04 => Some("Slot-identification"),
        0x05 => Some("MSI"),
        0x09 => Some("Vendor-specific"),
        0x0A => Some("Debug-port"),
        0x0C => Some("PCI Hot Plug"),
        0x0D => Some("PCI Bridge Subsystem ID"),
        0x10 => Some("PCIe"),
        0x11 => Some("MSI-X"),
        0x12 => Some("Serial ATA DATA/Index Configuration"),
        _ => None,
    }
}

pub fn name_of_extended_capability(type_: u16) -> Option<&'static str> {
    match type_ {
        0x0001 => Some("Advanced Error Reporting"),
        0x0002 => Some("Virtual Channel"),
        0x0003 => Some("Device Serial Number"),
        0x0004 => Some("Power Budgeting"),
        0x0005 => Some("Root Complex Link Declaration"),
        0x0006 => Some("Root Complex Internal Link Control"),
        0x000B => Some("Vendor-specific"),
        0x000D => Some("ACS"),
        0x000E => Some("ARI"),
        0x000F => Some("ATS"),
        0x0010 => Some("SR-IOV"),
        0x0011 => Some("MR-IOV"),
        0x0013 => Some("Page Request"),
        0x0015 => Some("Resizable BAR"),
        0x0017 => Some("TPH Requester"),
        0x0018 => Some("LTR"),
        0x001B => Some("PASID"),
        _ => None,
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum RoutingModel {
    None,
    RootTable,       // Routing table of PCI IRQ pins to global IRQs (i.e., PRT).
    ExpansionBridge, // Default routing of expansion bridges.
}

pub struct RoutingEntry {
    pub slot: u8,
    pub index: IrqIndex,
    pub pin: &'static IrqPin,
}

pub trait PciIrqRouter: Sync {
    fn resolve_irq_route(&self, slot: u8, index: IrqIndex) -> Option<&'static IrqPin>;

    fn make_downstream_router(&'static self, bus: &'static PciBus) -> &'static dyn PciIrqRouter;
}

// State shared by all PciIrqRouter implementations.
pub struct RouterState {
    pub routing_table: Vec<RoutingEntry>,
    pub routing_model: RoutingModel,
    pub bridge_irqs: [Option<&'static IrqPin>; 4],
}

impl RouterState {
    pub fn new() -> RouterState {
        RouterState {
            routing_table: Vec::new(),
            routing_model: RoutingModel::None,
            bridge_irqs: [None; 4],
        }
    }

    pub fn resolve_irq_route(&self, slot: u8, index: IrqIndex) -> Option<&'static IrqPin> {
        match self.routing_model {
            RoutingModel::RootTable => self
                .routing_table
                .iter()
                .find(|entry| entry.slot == slot && entry.index == index)
                .map(|entry| entry.pin),
            RoutingModel::ExpansionBridge => {
                self.bridge_irqs[(index as usize - 1 + slot as usize) % 4]
            }
            RoutingModel::None => None,
        }
    }

    pub fn route_expansion_bridge(&mut self, parent: &dyn PciIrqRouter, bus: &PciBus) {
        let bridge = bus
            .associated_bridge
            .expect("expansion bridge routing without an associated bridge");

        for (i, bridge_irq) in self.bridge_irqs.iter_mut().enumerate() {
            *bridge_irq =
                parent.resolve_irq_route(bridge.entity.slot, IrqIndex::from_pin(i as u8 + 1));
            if let Some(pin) = bridge_irq {
                println!("sif:     Bridge IRQ [{i}]: {}", pin.name());
            }
        }

        self.routing_model = RoutingModel::ExpansionBridge;
    }
}

pub struct PciBusResource {
    base: u64,
    size: u64,
    host_base: u64,
    flags: u32,
    alloc_offset: u64,
    is_host_mmio: bool,
}

impl PciBusResource {
    pub const IO: u32 = 1;
    pub const MEMORY: u32 = 2;
    pub const PREF_MEMORY: u32 = 3;

    pub fn new(
        base: u64,
        size: u64,
        host_base: u64,
        flags: u32,
        is_host_mmio: bool,
    ) -> PciBusResource {
        PciBusResource {
            base,
            size,
            host_base,
            flags,
            alloc_offset: 0,
            is_host_mmio,
        }
    }

    pub fn base(&self) -> u64 {
        self.base
    }

    pub fn size(&self) -> u64 {
        self.size
    }

    pub fn host_base(&self) -> u64 {
        self.host_base
    }

    pub fn flags(&self) -> u32 {
        self.flags
    }

    pub fn is_host_mmio(&self) -> bool {
        self.is_host_mmio
    }

    pub fn remaining(&self) -> u64 {
        self.size - self.alloc_offset
    }

    // Returns the offset from base on success.
    pub fn allocate(&mut self, size: u64) -> Option<u64> {
        // Size must be a power of 2.
        assert!(size & (size - 1) == 0);

        let tmp = (self.alloc_offset + size - 1) & !(size - 1);

        if tmp + size > self.size {
            return None;
        }

        self.alloc_offset = tmp + size;

        Some(tmp)
    }

    pub fn can_fit(&self, size: u64) -> bool {
        // Size must be a power of 2.
        assert!(size & (size - 1) == 0);

        let tmp = (self.alloc_offset + size - 1) & !(size - 1);

        tmp + size <= self.size
    }
}

pub struct PciBus {
    pub associated_bridge: Option<&'static PciBridge>,
    pub irq_router: OnceLock<&'static dyn PciIrqRouter>,
    pub io: &'static dyn PciConfigIo,
    pub child_devices: Mutex<Vec<&'static PciDevice>>,
    pub child_bridges: Mutex<Vec<&'static PciBridge>>,

    pub resources: Mutex<Vec<PciBusResource>>,

    pub seg_id: u16,
    pub bus_id: u8,

    // Whether DMA by devices below this bus snoops the CPU caches.
    pub dma_coherent: bool,

    pub mbus_id: AtomicI64,
}

// Every address that we hand out belongs to a device that we enumerated, hence accesses to it
// only fail if the caller picks a bad register.
const EXPECT_ACCESS: &str = "sif: configuration space access to an enumerated device failed";

impl PciBus {
    pub fn new(
        associated_bridge: Option<&'static PciBridge>,
        io: &'static dyn PciConfigIo,
        seg_id: u16,
        bus_id: u8,
        dma_coherent: bool,
    ) -> &'static PciBus {
        leak(PciBus {
            associated_bridge,
            irq_router: OnceLock::new(),
            io,
            child_devices: Mutex::new(Vec::new()),
            child_bridges: Mutex::new(Vec::new()),
            resources: Mutex::new(Vec::new()),
            seg_id,
            bus_id,
            dma_coherent,
            mbus_id: AtomicI64::new(0),
        })
    }

    pub fn make_downstream_bus(
        &'static self,
        bridge: &'static PciBridge,
        downstream_id: u8,
    ) -> &'static PciBus {
        let new_bus = PciBus::new(
            Some(bridge),
            self.io,
            self.seg_id,
            downstream_id,
            self.dma_coherent,
        );

        let router = self
            .irq_router
            .get()
            .expect("bus has no IRQ router")
            .make_downstream_router(new_bus);
        assert!(
            new_bus.irq_router.set(router).is_ok(),
            "sif: PCI bus already has an IRQ router"
        );

        new_bus
    }

    /// # Safety
    ///
    /// See [`config::PciConfigIo`].
    pub unsafe fn read_config_byte(&self, slot: u8, function: u8, offset: u16) -> u8 {
        unsafe {
            self.io
                .read_config_byte(self.seg_id, self.bus_id, slot, function, offset)
        }
        .expect(EXPECT_ACCESS)
    }

    /// # Safety
    ///
    /// See [`config::PciConfigIo`].
    pub unsafe fn read_config_half(&self, slot: u8, function: u8, offset: u16) -> u16 {
        unsafe {
            self.io
                .read_config_half(self.seg_id, self.bus_id, slot, function, offset)
        }
        .expect(EXPECT_ACCESS)
    }

    /// # Safety
    ///
    /// See [`config::PciConfigIo`].
    pub unsafe fn read_config_word(&self, slot: u8, function: u8, offset: u16) -> u32 {
        unsafe {
            self.io
                .read_config_word(self.seg_id, self.bus_id, slot, function, offset)
        }
        .expect(EXPECT_ACCESS)
    }

    /// # Safety
    ///
    /// See [`config::PciConfigIo`].
    pub unsafe fn write_config_byte(&self, slot: u8, function: u8, offset: u16, value: u8) {
        unsafe {
            self.io
                .write_config_byte(self.seg_id, self.bus_id, slot, function, offset, value)
        }
        .expect(EXPECT_ACCESS)
    }

    /// # Safety
    ///
    /// See [`config::PciConfigIo`].
    pub unsafe fn write_config_half(&self, slot: u8, function: u8, offset: u16, value: u16) {
        unsafe {
            self.io
                .write_config_half(self.seg_id, self.bus_id, slot, function, offset, value)
        }
        .expect(EXPECT_ACCESS)
    }

    /// # Safety
    ///
    /// See [`config::PciConfigIo`].
    pub unsafe fn write_config_word(&self, slot: u8, function: u8, offset: u16, value: u32) {
        unsafe {
            self.io
                .write_config_word(self.seg_id, self.bus_id, slot, function, offset, value)
        }
        .expect(EXPECT_ACCESS)
    }
}

// Accessors for registers whose side effects are known, hence they are safe.
impl PciBus {
    pub fn vendor(&self, slot: u8, function: u8) -> u16 {
        unsafe { self.read_config_half(slot, function, PCI_VENDOR) }
    }

    pub fn device_id(&self, slot: u8, function: u8) -> u16 {
        unsafe { self.read_config_half(slot, function, PCI_DEVICE) }
    }

    pub fn command(&self, slot: u8, function: u8) -> u16 {
        unsafe { self.read_config_half(slot, function, PCI_COMMAND) }
    }

    pub fn set_command(&self, slot: u8, function: u8, value: u16) {
        unsafe { self.write_config_half(slot, function, PCI_COMMAND, value) };
    }

    pub fn status(&self, slot: u8, function: u8) -> u16 {
        unsafe { self.read_config_half(slot, function, PCI_STATUS) }
    }

    pub fn revision(&self, slot: u8, function: u8) -> u8 {
        unsafe { self.read_config_byte(slot, function, PCI_REVISION) }
    }

    pub fn interface(&self, slot: u8, function: u8) -> u8 {
        unsafe { self.read_config_byte(slot, function, PCI_INTERFACE) }
    }

    pub fn sub_class(&self, slot: u8, function: u8) -> u8 {
        unsafe { self.read_config_byte(slot, function, PCI_SUB_CLASS) }
    }

    pub fn class_code(&self, slot: u8, function: u8) -> u8 {
        unsafe { self.read_config_byte(slot, function, PCI_CLASS_CODE) }
    }

    pub fn header_type(&self, slot: u8, function: u8) -> u8 {
        unsafe { self.read_config_byte(slot, function, PCI_HEADER_TYPE) }
    }

    pub fn capabilities_pointer(&self, slot: u8, function: u8) -> u8 {
        unsafe { self.read_config_byte(slot, function, PCI_REGULAR_CAPABILITIES) }
    }

    pub fn interrupt_pin(&self, slot: u8, function: u8) -> u8 {
        unsafe { self.read_config_byte(slot, function, PCI_REGULAR_INTERRUPT_PIN) }
    }
}

// Accessors for registers whose existence depends on the header type, hence they are unsafe.
impl PciBus {
    /// # Safety
    ///
    /// The function must have a PCI-to-PCI bridge header.
    pub unsafe fn secondary_bus(&self, slot: u8, function: u8) -> u8 {
        unsafe { self.read_config_byte(slot, function, PCI_BRIDGE_SECONDARY) }
    }

    /// # Safety
    ///
    /// The function must have a PCI-to-PCI bridge header.
    pub unsafe fn set_secondary_bus(&self, slot: u8, function: u8, value: u8) {
        unsafe { self.write_config_byte(slot, function, PCI_BRIDGE_SECONDARY, value) };
    }

    /// # Safety
    ///
    /// The function must have a PCI-to-PCI bridge header.
    pub unsafe fn subordinate_bus(&self, slot: u8, function: u8) -> u8 {
        unsafe { self.read_config_byte(slot, function, PCI_BRIDGE_SUBORDINATE) }
    }

    /// # Safety
    ///
    /// The function must have a PCI-to-PCI bridge header.
    pub unsafe fn set_subordinate_bus(&self, slot: u8, function: u8, value: u8) {
        unsafe { self.write_config_byte(slot, function, PCI_BRIDGE_SUBORDINATE, value) };
    }

    /// # Safety
    ///
    /// The function must have a PCI-to-PCI bridge header.
    pub unsafe fn io_base(&self, slot: u8, function: u8) -> u8 {
        unsafe { self.read_config_byte(slot, function, PCI_BRIDGE_IO_BASE) }
    }

    /// # Safety
    ///
    /// The function must have a PCI-to-PCI bridge header.
    pub unsafe fn set_io_base(&self, slot: u8, function: u8, value: u8) {
        unsafe { self.write_config_byte(slot, function, PCI_BRIDGE_IO_BASE, value) };
    }

    /// # Safety
    ///
    /// The function must have a PCI-to-PCI bridge header.
    pub unsafe fn io_limit(&self, slot: u8, function: u8) -> u8 {
        unsafe { self.read_config_byte(slot, function, PCI_BRIDGE_IO_LIMIT) }
    }

    /// # Safety
    ///
    /// The function must have a PCI-to-PCI bridge header.
    pub unsafe fn set_io_limit(&self, slot: u8, function: u8, value: u8) {
        unsafe { self.write_config_byte(slot, function, PCI_BRIDGE_IO_LIMIT, value) };
    }

    /// # Safety
    ///
    /// The function must have a PCI-to-PCI bridge header.
    pub unsafe fn mem_base(&self, slot: u8, function: u8) -> u16 {
        unsafe { self.read_config_half(slot, function, PCI_BRIDGE_MEM_BASE) }
    }

    /// # Safety
    ///
    /// The function must have a PCI-to-PCI bridge header.
    pub unsafe fn set_mem_base(&self, slot: u8, function: u8, value: u16) {
        unsafe { self.write_config_half(slot, function, PCI_BRIDGE_MEM_BASE, value) };
    }

    /// # Safety
    ///
    /// The function must have a PCI-to-PCI bridge header.
    pub unsafe fn mem_limit(&self, slot: u8, function: u8) -> u16 {
        unsafe { self.read_config_half(slot, function, PCI_BRIDGE_MEM_LIMIT) }
    }

    /// # Safety
    ///
    /// The function must have a PCI-to-PCI bridge header.
    pub unsafe fn set_mem_limit(&self, slot: u8, function: u8, value: u16) {
        unsafe { self.write_config_half(slot, function, PCI_BRIDGE_MEM_LIMIT, value) };
    }

    /// # Safety
    ///
    /// The function must have a PCI-to-PCI bridge header.
    pub unsafe fn prefetch_mem_base(&self, slot: u8, function: u8) -> u16 {
        unsafe { self.read_config_half(slot, function, PCI_BRIDGE_PREFETCH_MEM_BASE) }
    }

    /// # Safety
    ///
    /// The function must have a PCI-to-PCI bridge header.
    pub unsafe fn set_prefetch_mem_base(&self, slot: u8, function: u8, value: u16) {
        unsafe { self.write_config_half(slot, function, PCI_BRIDGE_PREFETCH_MEM_BASE, value) };
    }

    /// # Safety
    ///
    /// The function must have a PCI-to-PCI bridge header.
    pub unsafe fn prefetch_mem_limit(&self, slot: u8, function: u8) -> u16 {
        unsafe { self.read_config_half(slot, function, PCI_BRIDGE_PREFETCH_MEM_LIMIT) }
    }

    /// # Safety
    ///
    /// The function must have a PCI-to-PCI bridge header.
    pub unsafe fn set_prefetch_mem_limit(&self, slot: u8, function: u8, value: u16) {
        unsafe { self.write_config_half(slot, function, PCI_BRIDGE_PREFETCH_MEM_LIMIT, value) };
    }

    /// # Safety
    ///
    /// The function must have a PCI-to-PCI bridge header.
    pub unsafe fn prefetch_mem_base_upper(&self, slot: u8, function: u8) -> u32 {
        unsafe { self.read_config_word(slot, function, PCI_BRIDGE_PREFETCH_MEM_BASE_UPPER) }
    }

    /// # Safety
    ///
    /// The function must have a PCI-to-PCI bridge header.
    pub unsafe fn set_prefetch_mem_base_upper(&self, slot: u8, function: u8, value: u32) {
        unsafe {
            self.write_config_word(slot, function, PCI_BRIDGE_PREFETCH_MEM_BASE_UPPER, value)
        };
    }

    /// # Safety
    ///
    /// The function must have a PCI-to-PCI bridge header.
    pub unsafe fn prefetch_mem_limit_upper(&self, slot: u8, function: u8) -> u32 {
        unsafe { self.read_config_word(slot, function, PCI_BRIDGE_PREFETCH_MEM_LIMIT_UPPER) }
    }

    /// # Safety
    ///
    /// The function must have a PCI-to-PCI bridge header.
    pub unsafe fn set_prefetch_mem_limit_upper(&self, slot: u8, function: u8, value: u32) {
        unsafe {
            self.write_config_word(slot, function, PCI_BRIDGE_PREFETCH_MEM_LIMIT_UPPER, value)
        };
    }

    /// # Safety
    ///
    /// The function must have a regular header.
    pub unsafe fn subsystem_vendor(&self, slot: u8, function: u8) -> u16 {
        unsafe { self.read_config_half(slot, function, PCI_REGULAR_SUBSYSTEM_VENDOR) }
    }

    /// # Safety
    ///
    /// The function must have a regular header.
    pub unsafe fn subsystem_device(&self, slot: u8, function: u8) -> u16 {
        unsafe { self.read_config_half(slot, function, PCI_REGULAR_SUBSYSTEM_DEVICE) }
    }

    // Only the first two BARs exist in both header types.
    fn check_bar(offset: u16) {
        assert!(offset >= PCI_REGULAR_BAR0 && offset < PCI_REGULAR_BAR0 + 24);
        assert!(offset % 4 == 0);
    }

    /// # Safety
    ///
    /// The offset must address a BAR of the header type of the function.
    pub unsafe fn bar(&self, slot: u8, function: u8, offset: u16) -> u32 {
        Self::check_bar(offset);
        unsafe { self.read_config_word(slot, function, offset) }
    }

    /// # Safety
    ///
    /// The offset must address a BAR of the header type of the function.
    pub unsafe fn set_bar(&self, slot: u8, function: u8, offset: u16, value: u32) {
        Self::check_bar(offset);
        unsafe { self.write_config_word(slot, function, offset, value) };
    }

    // Both header types have an expansion ROM register, but at different offsets.
    fn check_expansion_rom(offset: u16) {
        assert!(
            offset == PCI_REGULAR_EXPANSION_ROM_BASE_ADDRESS
                || offset == PCI_BRIDGE_EXPANSION_ROM_BASE_ADDRESS
        );
    }

    /// # Safety
    ///
    /// The offset must address the expansion ROM of the header type of the function.
    pub unsafe fn expansion_rom_base(&self, slot: u8, function: u8, offset: u16) -> u32 {
        Self::check_expansion_rom(offset);
        unsafe { self.read_config_word(slot, function, offset) }
    }

    /// # Safety
    ///
    /// The offset must address the expansion ROM of the header type of the function.
    pub unsafe fn set_expansion_rom_base(&self, slot: u8, function: u8, offset: u16, value: u32) {
        Self::check_expansion_rom(offset);
        unsafe { self.write_config_word(slot, function, offset, value) };
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Default, Debug)]
pub enum BarType {
    #[default]
    None,
    Io,
    Memory,
}

#[derive(Clone, Copy, Default)]
pub struct PciBar {
    pub type_: BarType,
    pub address: u64,
    pub length: u64,
    pub prefetchable: bool,

    pub allocated: bool,
    pub host_type: BarType,
    pub host_address: u64,
    pub offset: u32,
}

#[derive(Clone, Copy, Default)]
pub struct PciExpansionRom {
    pub address: u64,
    pub length: u64,
}

pub struct Capability {
    pub type_: u32,
    pub offset: u16,
    pub length: Option<u64>,
}

// Entry of the MSI-X vector table, from the PCI specification.
const MSIX_ENTRY_SIZE: usize = 16;
scalar_register!(MsixMessageAddress @ 0: u64);
scalar_register!(MsixMessageData @ 8: u32);
bit_register! {
    MsixVectorControl @ 12: u32 {
        MASK @ 0, 1: bool;
    }
}

// MSI-X vector table of a device, mapped from the BAR named by the MSI-X capability.
pub struct MsixTable {
    mapping: hel::Mapping<u8>,
    disp: usize,
}

impl MsixTable {
    pub fn new(mapping: hel::Mapping<u8>, disp: usize) -> MsixTable {
        MsixTable { mapping, disp }
    }

    // Returns the window of the vector table entry at `index`.
    fn entry(&self, index: usize) -> IoMemSpace {
        let space =
            unsafe { IoMemSpace::new(self.mapping.as_ptr().unwrap().as_ptr(), self.mapping.len()) };
        space.subspace(self.disp + index * MSIX_ENTRY_SIZE)
    }

    pub fn store_message_address(&self, index: usize, value: u64) {
        unsafe { self.entry(index).store(MsixMessageAddress, value) };
    }

    pub fn store_message_data(&self, index: usize, value: u32) {
        unsafe { self.entry(index).store(MsixMessageData, value) };
    }

    pub fn set_masked(&self, index: usize, masked: bool) {
        let entry = self.entry(index);
        let control = unsafe { entry.load(MsixVectorControl) };
        unsafe {
            entry.store(
                MsixVectorControl,
                control.with(MsixVectorControl::MASK, masked),
            )
        };
    }
}

// Not consumed yet; kept to match the information thor collects.
#[allow(dead_code)]
pub struct ExtendedCapability {
    pub type_: u16,
    pub offset: u16,
}

// Common part of devices and bridges.
pub struct PciEntity {
    pub parent_bus: &'static PciBus,

    // Location of the entity on the PCI bus.
    pub seg: u16,
    pub bus: u8,
    pub slot: u8,
    pub function: u8,

    // mbus object ID of the entity.
    pub mbus_id: AtomicI64,

    // vendor-specific device information
    pub vendor: u16,
    pub device_id: u16,
    pub revision: u8,

    // generic device information
    pub class_code: u8,
    pub sub_class: u8,
    pub interface: u8,

    pub is_pcie: AtomicBool,
    // Device/port type of the PCI Express capability. Only meaningful if is_pcie is set.
    pub pcie_port_type: AtomicU8,

    pub caps: Mutex<Vec<Capability>>,
    pub extended_caps: Mutex<Vec<ExtendedCapability>>,

    pub expansion_rom: OnceLock<PciExpansionRom>,

    pub bars: Mutex<Vec<PciBar>>,

    // IOMMU that translates the DMA requests of this entity, and the domain that it is bound to.
    pub associated_iommu: OnceLock<&'static iommu::IommuUnit>,
    pub dma_domain: OnceLock<&'static iommu::DmaDomain>,
    // Stands in for the domain if no IOMMU translates the entity.
    pub noop_dma_space: OnceLock<hel::Handle>,
}

impl PciEntity {
    #[allow(clippy::too_many_arguments)]
    fn new(
        parent_bus: &'static PciBus,
        slot: u8,
        function: u8,
        vendor: u16,
        device_id: u16,
        revision: u8,
        class_code: u8,
        sub_class: u8,
        interface: u8,
        n_bars: usize,
    ) -> PciEntity {
        PciEntity {
            parent_bus,
            seg: parent_bus.seg_id,
            bus: parent_bus.bus_id,
            slot,
            function,
            mbus_id: AtomicI64::new(0),
            vendor,
            device_id,
            revision,
            class_code,
            sub_class,
            interface,
            is_pcie: AtomicBool::new(false),
            pcie_port_type: AtomicU8::new(0),
            caps: Mutex::new(Vec::new()),
            extended_caps: Mutex::new(Vec::new()),
            expansion_rom: OnceLock::new(),
            bars: Mutex::new(vec![PciBar::default(); n_bars]),
            associated_iommu: OnceLock::new(),
            dma_domain: OnceLock::new(),
            noop_dma_space: OnceLock::new(),
        }
    }

    /// Whether the entity is a PCIe port that can only have a single device attached to it.
    pub fn is_downstream_port(&self) -> bool {
        self.is_pcie.load(Ordering::Relaxed)
            && matches!(
                self.pcie_port_type.load(Ordering::Relaxed),
                PCIE_TYPE_ROOT_PORT | PCIE_TYPE_DOWNSTREAM_PORT | PCIE_TYPE_PCI_TO_PCIE_BRIDGE
            )
    }

    pub fn enable_busmaster(&self) {
        // Enable busmastering for the whole tree.
        let mut entity = self;
        loop {
            let bus = entity.parent_bus;

            let cmd = bus.command(entity.slot, entity.function);
            bus.set_command(entity.slot, entity.function, cmd | 0x0004);

            match bus.associated_bridge {
                Some(bridge) => entity = &bridge.entity,
                None => break,
            }
        }
    }
}

pub struct PciBridge {
    pub entity: PciEntity,

    pub associated_bus: OnceLock<&'static PciBus>,
    pub downstream_id: AtomicU8,
    pub subordinate_id: AtomicU8,
}

impl PciBridge {
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        parent_bus: &'static PciBus,
        slot: u8,
        function: u8,
        vendor: u16,
        device_id: u16,
        revision: u8,
        class_code: u8,
        sub_class: u8,
        interface: u8,
    ) -> &'static PciBridge {
        leak(PciBridge {
            entity: PciEntity::new(
                parent_bus, slot, function, vendor, device_id, revision, class_code, sub_class,
                interface, 2,
            ),
            associated_bus: OnceLock::new(),
            downstream_id: AtomicU8::new(0),
            subordinate_id: AtomicU8::new(0),
        })
    }
}

pub struct PciDevice {
    pub entity: PciEntity,

    pub subsystem_vendor: u16,
    pub subsystem_device: u16,

    pub interrupt: OnceLock<&'static IrqPin>,

    // Physical address and size of the Intel IGD VBT, if any.
    pub igd_vbt: OnceLock<(u64, u64)>,

    // MSI / MSI-X support.
    // Only set once the vectors were fully set up (e.g. the MSI-X table could be mapped).
    pub msis_usable: AtomicBool,
    pub num_msis: AtomicU32,
    pub msix_index: AtomicI32,
    pub msix_mapping: Mutex<Option<MsixTable>>,
    pub msi_index: AtomicI32,
    pub msi_enabled: AtomicBool,
    pub msi_installed: AtomicBool,
}

impl PciDevice {
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        parent_bus: &'static PciBus,
        slot: u8,
        function: u8,
        vendor: u16,
        device_id: u16,
        revision: u8,
        class_code: u8,
        sub_class: u8,
        interface: u8,
        subsystem_vendor: u16,
        subsystem_device: u16,
    ) -> &'static PciDevice {
        leak(PciDevice {
            entity: PciEntity::new(
                parent_bus, slot, function, vendor, device_id, revision, class_code, sub_class,
                interface, 6,
            ),
            subsystem_vendor,
            subsystem_device,
            interrupt: OnceLock::new(),
            igd_vbt: OnceLock::new(),
            msis_usable: AtomicBool::new(false),
            num_msis: AtomicU32::new(0),
            msix_index: AtomicI32::new(-1),
            msix_mapping: Mutex::new(None),
            msi_index: AtomicI32::new(-1),
            msi_enabled: AtomicBool::new(false),
            msi_installed: AtomicBool::new(false),
        })
    }

    pub fn enable_irq(&self) {
        let bus = self.entity.parent_bus;

        let command = bus.command(self.entity.slot, self.entity.function);
        bus.set_command(self.entity.slot, self.entity.function, command & !0x400);
    }

    pub fn setup_msi(&self, msi: &hel::MsiInfo, index: usize) {
        let bus = self.entity.parent_bus;
        let slot = self.entity.slot;
        let function = self.entity.function;

        if self.msix_index.load(Ordering::Relaxed) >= 0 {
            // Setup the MSI-X table.
            let msix_mapping = self.msix_mapping.lock().expect(EXPECT_LOCK);
            let table = msix_mapping
                .as_ref()
                .expect("sif: MSI-X table is not mapped");
            table.store_message_address(index, msi.address);
            table.store_message_data(index, msi.data);
            table.set_masked(index, false);
        } else {
            let msi_index = self.msi_index.load(Ordering::Relaxed);
            assert!(msi_index >= 0);

            // TODO(qookie): support non-zero indices
            assert!(index == 0);
            let offset = self.entity.caps.lock().expect(EXPECT_LOCK)[msi_index as usize].offset;

            let mut msg_control = unsafe { bus.read_config_half(slot, function, offset + 2) };

            let is_64_capable = msg_control & (1 << 7) != 0;

            unsafe { bus.write_config_word(slot, function, offset + 4, msi.address as u32) };

            if is_64_capable {
                unsafe {
                    bus.write_config_word(slot, function, offset + 8, (msi.address >> 32) as u32)
                };
                unsafe { bus.write_config_half(slot, function, offset + 12, msi.data as u16) };
            } else {
                assert!(msi.address >> 32 == 0);
                unsafe { bus.write_config_half(slot, function, offset + 8, msi.data as u16) };
            }

            if self.msi_enabled.load(Ordering::Relaxed) {
                // Enable MSI
                msg_control |= 0x0001;

                unsafe { bus.write_config_half(slot, function, offset + 2, msg_control) };
            }

            self.msi_installed.store(true, Ordering::Relaxed);
        }
    }

    // Unlike thor, this does not unmask INTx: clients that use INTx alongside MSIs
    // (e.g. virtio for configuration changes) enable it explicitly via EnableBusIrq.
    pub fn enable_msi(&self) {
        let bus = self.entity.parent_bus;
        let slot = self.entity.slot;
        let function = self.entity.function;

        let msix_index = self.msix_index.load(Ordering::Relaxed);
        if msix_index >= 0 {
            let offset = self.entity.caps.lock().expect(EXPECT_LOCK)[msix_index as usize].offset;

            let mut msg_control = unsafe { bus.read_config_half(slot, function, offset + 2) };

            msg_control |= 0x8000; // Enable MSI-X.

            msg_control &= !0x4000; // Disable the overall mask.
            unsafe { bus.write_config_half(slot, function, offset + 2, msg_control) };
        } else {
            let msi_index = self.msi_index.load(Ordering::Relaxed);
            assert!(msi_index >= 0);

            let offset = self.entity.caps.lock().expect(EXPECT_LOCK)[msi_index as usize].offset;

            let mut msg_control = unsafe { bus.read_config_half(slot, function, offset + 2) };

            if !self.msi_installed.load(Ordering::Relaxed) {
                // Disable MSI by default, configure to only 1 message
                // MSIs will be reenabled once one is installed in setup_msi, since
                // we may not have a way to mask it otherwise (mask register only
                // exists if MSIs are 64-bit).
                msg_control &= !0x0071;
            } else {
                // setup_msi was called before enable_msi, so we can enable them
                // without worrying about needing the MSI to be masked.
                msg_control &= !0x0070; // Only one message
                msg_control |= 0x0001; // Enable MSI
            }

            unsafe { bus.write_config_half(slot, function, offset + 2, msg_control) };

            self.msi_enabled.store(true, Ordering::Relaxed);
        }
    }
}

// general PCI header fields
pub const PCI_VENDOR: u16 = 0;
pub const PCI_DEVICE: u16 = 2;
pub const PCI_COMMAND: u16 = 4;
pub const PCI_STATUS: u16 = 6;
pub const PCI_REVISION: u16 = 0x08;
pub const PCI_INTERFACE: u16 = 0x09;
pub const PCI_SUB_CLASS: u16 = 0x0A;
pub const PCI_CLASS_CODE: u16 = 0x0B;
pub const PCI_HEADER_TYPE: u16 = 0x0E;

// usual device header fields
pub const PCI_REGULAR_BAR0: u16 = 0x10;
pub const PCI_REGULAR_SUBSYSTEM_VENDOR: u16 = 0x2C;
pub const PCI_REGULAR_SUBSYSTEM_DEVICE: u16 = 0x2E;
pub const PCI_REGULAR_EXPANSION_ROM_BASE_ADDRESS: u16 = 0x30;
pub const PCI_REGULAR_CAPABILITIES: u16 = 0x34;
pub const PCI_REGULAR_INTERRUPT_PIN: u16 = 0x3D;

// Device/port types of the PCI Express capability.
pub const PCIE_TYPE_ROOT_PORT: u8 = 0x4;
pub const PCIE_TYPE_UPSTREAM_PORT: u8 = 0x5;
pub const PCIE_TYPE_DOWNSTREAM_PORT: u8 = 0x6;
pub const PCIE_TYPE_PCIE_TO_PCI_BRIDGE: u8 = 0x7;
pub const PCIE_TYPE_PCI_TO_PCIE_BRIDGE: u8 = 0x8;

// PCI-to-PCI bridge header fields
pub const PCI_BRIDGE_EXPANSION_ROM_BASE_ADDRESS: u16 = 0x38;
pub const PCI_BRIDGE_IO_BASE: u16 = 0x1C;
pub const PCI_BRIDGE_IO_LIMIT: u16 = 0x1D;
pub const PCI_BRIDGE_MEM_BASE: u16 = 0x20;
pub const PCI_BRIDGE_MEM_LIMIT: u16 = 0x22;
pub const PCI_BRIDGE_PREFETCH_MEM_BASE: u16 = 0x24;
pub const PCI_BRIDGE_PREFETCH_MEM_LIMIT: u16 = 0x26;
pub const PCI_BRIDGE_PREFETCH_MEM_BASE_UPPER: u16 = 0x28;
pub const PCI_BRIDGE_PREFETCH_MEM_LIMIT_UPPER: u16 = 0x2C;
pub const PCI_BRIDGE_SECONDARY: u16 = 0x19;
pub const PCI_BRIDGE_SUBORDINATE: u16 = 0x1A;

pub async fn publish_devices() -> Result<()> {
    // Each discovery source no-ops if its firmware interface is absent. ACPI systems
    // describe PCI via the MCFG even when a device tree is also present.
    acpi::discover_root_buses();
    if !crate::acpi::has_rsdp() {
        dtb::discover_root_buses();
    }

    discover::enumerate_all();
    // Every requester has to be bound before a device is published: a driver must not see a
    // translated DMA space before the IOMMU translates.
    iommu::configure().await;
    serve::publish_all().await?;

    Ok(())
}
