use managarm::svrctl::hardware_access_handle;
use std::collections::HashMap;
use std::sync::Arc;
use std::sync::atomic::Ordering;

use anyhow::Result;
use managarm::hw::pci::IoType;
use managarm::hw::server::{BarDescriptor, CapDescriptor, serve_pci_device};
use managarm::mbus::{EntityManager, Properties, create_entity};

use super::discover::{all_devices, all_root_buses};
use super::{
    BarType, EXPECT_LOCK, PciBridge, PciBus, PciDevice, PciEntity, iommu, leak,
    msi_controller_available,
};

use crate::acpi::{PAGE_MASK, PAGE_SIZE};
use crate::entity::{decimal, hex, serve_entity_lanes, string};

#[derive(Clone, Copy)]
enum ServedEntity {
    Device(&'static PciDevice),
    Bridge(&'static PciBridge),
}

impl ServedEntity {
    fn entity(&self) -> &'static PciEntity {
        match self {
            ServedEntity::Device(device) => &device.entity,
            ServedEntity::Bridge(bridge) => &bridge.entity,
        }
    }
}

fn io_type_of(type_: BarType) -> IoType {
    match type_ {
        BarType::None => IoType::None,
        BarType::Io => IoType::Port,
        BarType::Memory => IoType::Memory,
    }
}

impl managarm::hw::server::PciDevice for ServedEntity {
    fn num_msis(&self) -> u32 {
        let ServedEntity::Device(device) = self else {
            return 0;
        };
        if !msi_controller_available() || !device.msis_usable.load(Ordering::Relaxed) {
            return 0;
        }
        device.num_msis.load(Ordering::Relaxed)
    }

    fn msi_x(&self) -> bool {
        let ServedEntity::Device(device) = self else {
            return false;
        };
        device.msix_index.load(Ordering::Relaxed) >= 0
    }

    fn bars(&self) -> [BarDescriptor; 6] {
        let mut out = [BarDescriptor::default(); 6];
        for (i, bar) in self
            .entity()
            .bars
            .lock()
            .expect(EXPECT_LOCK)
            .iter()
            .enumerate()
        {
            out[i] = BarDescriptor {
                io_type: io_type_of(bar.type_),
                host_type: io_type_of(bar.host_type),
                address: bar.address,
                length: bar.length,
                offset: bar.offset,
            };
        }
        out
    }

    fn expansion_rom(&self) -> (u64, u64) {
        match self.entity().expansion_rom.get() {
            Some(rom) => (rom.address, rom.length),
            None => (0, 0),
        }
    }

    fn access_expansion_rom(&self) -> hel::Result<Option<hel::Handle>> {
        let Some(rom) = self.entity().expansion_rom.get() else {
            return Ok(None);
        };
        if rom.address == 0 {
            return Ok(None);
        }

        let aligned = (rom.address as usize) & !PAGE_MASK;
        let page_off = (rom.address as usize) & PAGE_MASK;
        let span = ((rom.length as usize) + page_off + PAGE_MASK) & !PAGE_MASK;
        // Some cards have problems with caching the expansion ROM.
        Ok(Some(hel::access_physical(
            hardware_access_handle(),
            aligned,
            span,
            hel::CachingMode::Uncached,
        )?))
    }

    fn capabilities(&self) -> Vec<CapDescriptor> {
        self.entity()
            .caps
            .lock()
            .expect(EXPECT_LOCK)
            .iter()
            .map(|cap| CapDescriptor {
                type_: cap.type_,
                offset: cap.offset as u64,
                length: cap.length.unwrap_or(!0),
            })
            .collect()
    }

    fn config_read(&self, offset: u32, size: u32) -> Option<u32> {
        let entity = self.entity();
        let bus = entity.parent_bus;

        let limit = if bus.io.supports_4k_config_space() {
            0x1000
        } else {
            0x100
        };
        // The offset comes from the client, hence a wrapping addition would defeat the check.
        if !matches!(size, 1 | 2 | 4)
            || offset & (size - 1) != 0
            || offset.checked_add(size).is_none_or(|end| end > limit)
        {
            return None;
        }

        let offset = offset as u16;
        // The protocol hands out raw config space access; the client is responsible for it.
        Some(match size {
            1 => (unsafe { bus.read_config_byte(entity.slot, entity.function, offset) }) as u32,
            2 => (unsafe { bus.read_config_half(entity.slot, entity.function, offset) }) as u32,
            _ => unsafe { bus.read_config_word(entity.slot, entity.function, offset) },
        })
    }

    fn config_write(&self, offset: u32, size: u32, word: u32) -> bool {
        let entity = self.entity();
        let bus = entity.parent_bus;

        let limit = if bus.io.supports_4k_config_space() {
            0x1000
        } else {
            0x100
        };
        // The offset comes from the client, hence a wrapping addition would defeat the check.
        if !matches!(size, 1 | 2 | 4)
            || offset & (size - 1) != 0
            || offset.checked_add(size).is_none_or(|end| end > limit)
        {
            return false;
        }

        let offset = offset as u16;
        // The protocol hands out raw config space access; the client is responsible for it.
        match size {
            1 => unsafe { bus.write_config_byte(entity.slot, entity.function, offset, word as u8) },
            2 => unsafe {
                bus.write_config_half(entity.slot, entity.function, offset, word as u16)
            },
            _ => unsafe { bus.write_config_word(entity.slot, entity.function, offset, word) },
        }
        true
    }

    fn capability_read(&self, index: i32, offset: u32, size: u32) -> Option<u32> {
        let cap_offset = {
            let caps = self.entity().caps.lock().expect(EXPECT_LOCK);
            caps.get(usize::try_from(index).ok()?)?.offset
        };
        self.config_read(u32::from(cap_offset).checked_add(offset)?, size)
    }

    fn access_bar(&self, index: usize) -> hel::Result<hel::Handle> {
        let bars = self.entity().bars.lock().expect(EXPECT_LOCK);
        let bar = bars.get(index).ok_or(hel::Error::IllegalArgs)?;
        match bar.host_type {
            BarType::Memory => {
                let aligned = (bar.host_address as usize) & !PAGE_MASK;
                let page_off = (bar.host_address as usize) & PAGE_MASK;
                let span = ((bar.length as usize) + page_off + PAGE_MASK) & !PAGE_MASK;
                hel::access_physical(
                    hardware_access_handle(),
                    aligned,
                    span.max(PAGE_SIZE),
                    hel::CachingMode::Mmio,
                )
            }
            BarType::Io => {
                let ports: Vec<usize> = (bar.address..bar.address + bar.length)
                    .map(|p| p as usize)
                    .collect();
                hel::access_io(hardware_access_handle(), &ports)
            }
            BarType::None => Err(hel::Error::IllegalArgs),
        }
    }

    fn access_vbt(&self) -> hel::Result<Option<(u32, hel::Handle)>> {
        let ServedEntity::Device(device) = self else {
            return Ok(None);
        };
        let Some(&(address, size)) = device.igd_vbt.get() else {
            return Ok(None);
        };

        let aligned = (address as usize) & !PAGE_MASK;
        let span = ((size as usize) + PAGE_MASK) & !PAGE_MASK;
        Ok(Some((
            span as u32,
            hel::access_physical(
                hardware_access_handle(),
                aligned,
                span,
                hel::CachingMode::Uncached,
            )?,
        )))
    }

    fn access_irq(&self, index: u64) -> hel::Result<Option<hel::Handle>> {
        let ServedEntity::Device(device) = self else {
            return Ok(None);
        };
        if index != 0 {
            return Ok(None);
        }
        match device.interrupt.get() {
            Some(pin) => Ok(Some(hel::handle_irq(pin.handle())?)),
            None => Ok(None),
        }
    }

    fn install_msi(&self, index: u32) -> hel::Result<Option<hel::Handle>> {
        let ServedEntity::Device(device) = self else {
            return Ok(None);
        };
        if !msi_controller_available()
            || !device.msis_usable.load(Ordering::Relaxed)
            || index >= device.num_msis.load(Ordering::Relaxed)
        {
            return Ok(None);
        }

        // Allocate the MSI.
        let entity = &device.entity;
        let name = format!(
            "pci-msi.{:04x}:{:02x}:{:02x}.{:x}.{index}",
            entity.seg, entity.bus, entity.slot, entity.function
        );
        let pin = hel::allocate_msi(hardware_access_handle(), &name)?;
        let msi = hel::query_msi_info(&pin)?;

        // Obtain an IRQ object for the interrupt.
        let irq = hel::handle_irq(&pin)?;

        device.setup_msi(&msi, index as usize);

        Ok(Some(irq))
    }

    fn enable_busmaster(&self) {
        self.entity().enable_busmaster();
    }

    fn enable_irq(&self) {
        if let ServedEntity::Device(device) = self {
            device.enable_irq();
        }
    }

    fn enable_msi(&self) -> bool {
        let ServedEntity::Device(device) = self else {
            return false;
        };
        if !msi_controller_available() || !device.msis_usable.load(Ordering::Relaxed) {
            return false;
        }
        device.enable_msi();
        true
    }

    async fn enable_dma(&self, passthrough: bool) -> bool {
        let entity = self.entity();
        let Some(unit) = iommu::find_iommu(entity) else {
            return false;
        };
        // Without a domain there is nothing to translate through, hence the request fails
        // instead of falling back to passthrough.
        let domain = if passthrough {
            None
        } else {
            let Some(&domain) = entity.dma_domain.get() else {
                return false;
            };
            Some(domain)
        };
        iommu::bind_device(unit, domain, entity).await
    }

    fn get_dma_space(&self) -> hel::Result<(bool, hel::Handle)> {
        iommu::dma_space(self.entity())
    }

    fn dma_coherent(&self) -> bool {
        self.entity().parent_bus.dma_coherent
    }
}

async fn serve_entity(manager: &'static EntityManager, served: ServedEntity) {
    let device = Arc::new(served);

    serve_entity_lanes(manager, move |lane| {
        hel::spawn(serve_pci_device(lane, device.clone()));
    })
    .await;
}

fn entity_properties(entity: &PciEntity, pci_type: &str) -> Properties {
    let mut props = HashMap::new();
    props.insert("unix.subsystem".into(), string("pci"));
    props.insert("pci-type".into(), string(pci_type));
    props.insert("pci-segment".into(), hex(entity.seg as u32, 4));
    props.insert("pci-bus".into(), hex(entity.bus as u32, 2));
    props.insert("pci-slot".into(), hex(entity.slot as u32, 2));
    props.insert("pci-function".into(), hex(entity.function as u32, 1));
    props.insert("pci-vendor".into(), hex(entity.vendor as u32, 4));
    props.insert("pci-device".into(), hex(entity.device_id as u32, 4));
    props.insert("pci-revision".into(), hex(entity.revision as u32, 2));
    props.insert("pci-class".into(), hex(entity.class_code as u32, 2));
    props.insert("pci-subclass".into(), hex(entity.sub_class as u32, 2));
    props.insert("pci-interface".into(), hex(entity.interface as u32, 2));

    let parent = entity
        .parent_bus
        .associated_bridge
        .map(|bridge| bridge.entity.mbus_id.load(Ordering::Relaxed))
        .unwrap_or(entity.parent_bus.mbus_id.load(Ordering::Relaxed));
    props.insert("drvcore.mbus-parent".into(), decimal(parent));

    props
}

async fn publish_entity(served: ServedEntity, pci_type: &str) -> Result<()> {
    let entity = served.entity();

    let mut props = entity_properties(entity, pci_type);
    if let ServedEntity::Device(device) = served {
        props.insert(
            "pci-subsystem-vendor".into(),
            hex(device.subsystem_vendor as u32, 2),
        );
        props.insert(
            "pci-subsystem-device".into(),
            hex(device.subsystem_device as u32, 2),
        );
    }

    println!(
        "sif: pci: {:04x}:{:02x}:{:02x}.{} {} vendor: {:04x} device: {:04x}",
        entity.seg,
        entity.bus,
        entity.slot,
        entity.function,
        pci_type,
        entity.vendor,
        entity.device_id,
    );

    let manager: &'static EntityManager = leak(create_entity(pci_type, &props).await?);
    entity.mbus_id.store(manager.id(), Ordering::Relaxed);
    hel::spawn(serve_entity(manager, served));

    Ok(())
}

fn bridges_in_publish_order(bus: &'static PciBus, out: &mut Vec<&'static PciBridge>) {
    for bridge in bus.child_bridges.lock().expect(EXPECT_LOCK).iter() {
        out.push(bridge);
        if let Some(&downstream) = bridge.associated_bus.get() {
            bridges_in_publish_order(downstream, out);
        }
    }
}

pub async fn publish_all() -> Result<()> {
    for root_bus in all_root_buses() {
        let mut props: Properties = HashMap::new();
        props.insert("unix.subsystem".into(), string("pci"));
        props.insert("pci-type".into(), string("pci-root-bus"));
        props.insert("pci-segment".into(), hex(root_bus.seg_id as u32, 4));
        props.insert("pci-bus".into(), hex(root_bus.bus_id as u32, 2));

        println!(
            "sif: PCI root bus {:04x}:{:02x}",
            root_bus.seg_id, root_bus.bus_id
        );
        let manager: &'static EntityManager = leak(create_entity("pci-root-bus", &props).await?);
        root_bus.mbus_id.store(manager.id(), Ordering::Relaxed);

        // Publish bridges in pre-order so that every bridge finds its parent's mbus ID set.
        let mut bridges = Vec::new();
        bridges_in_publish_order(root_bus, &mut bridges);
        for bridge in bridges {
            publish_entity(ServedEntity::Bridge(bridge), "pci-bridge").await?;
        }
    }

    for device in all_devices() {
        publish_entity(ServedEntity::Device(device), "pci-device").await?;
    }

    Ok(())
}
