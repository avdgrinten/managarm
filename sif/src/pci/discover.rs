use std::sync::Mutex;
use std::sync::atomic::Ordering;

use managarm::svrctl::hardware_access_handle;

use managarm::kerncfg::{KernelDeviceResource, KernelDeviceResourceKind};

use super::{
    BarType, Capability, ExtendedCapability, IrqIndex, MsixTable,
    PCI_BRIDGE_EXPANSION_ROM_BASE_ADDRESS, PCI_REGULAR_BAR0,
    PCI_REGULAR_EXPANSION_ROM_BASE_ADDRESS, PciBridge, PciBus, PciBusResource, PciDevice,
    PciEntity, PciError, PciExpansionRom, name_of_capability, name_of_extended_capability,
};

use crate::EXPECT_LOCK;
use crate::acpi::PAGE_MASK;
use crate::kernel_device;

// Capability lists are walked with a bound since a device can make them loop; Linux uses the
// same bounds.
const MAX_CAPABILITIES: usize = 48;
const MAX_EXTENDED_CAPABILITIES: usize = (0x1000 - 0x100) / 8;

static ALL_DEVICES: Mutex<Vec<&'static PciDevice>> = Mutex::new(Vec::new());
static ALL_ROOT_BUSES: Mutex<Vec<&'static PciBus>> = Mutex::new(Vec::new());

pub fn all_devices() -> Vec<&'static PciDevice> {
    ALL_DEVICES.lock().expect(EXPECT_LOCK).clone()
}

pub fn all_root_buses() -> Vec<&'static PciBus> {
    ALL_ROOT_BUSES.lock().expect(EXPECT_LOCK).clone()
}

pub fn add_root_bus(bus: &'static PciBus) {
    ALL_ROOT_BUSES.lock().expect(EXPECT_LOCK).push(bus);
}

fn compute_bar_length(mask: u64) -> u64 {
    assert!(mask != 0);
    1u64 << mask.trailing_zeros()
}

fn bar_type_of(kind: KernelDeviceResourceKind) -> BarType {
    match kind {
        KernelDeviceResourceKind::Io => BarType::Io,
        KernelDeviceResourceKind::Mmio => BarType::Memory,
    }
}

fn range_contains(base: u64, size: u64, resource: &KernelDeviceResource) -> bool {
    let Some(end) = base.checked_add(size) else {
        return false;
    };
    let Some(resource_end) = resource.base.checked_add(resource.length) else {
        return false;
    };
    base <= resource.base && resource_end <= end
}

fn read_entity_bars(entity: &'static PciEntity, n_bars: usize) -> Result<(), PciError> {
    let bus = entity.parent_bus;
    let slot = entity.slot;
    let function = entity.function;

    let mut bars = entity.bars.lock().expect(EXPECT_LOCK);

    // The caller passes the number of BARs of the header type of the entity, hence all
    // offsets that we compute below address BARs.
    //
    // Sizing a BAR temporarily invalidates its address; disable I/O and memory
    // decode around the probe so the device does not respond to stray accesses.
    let size_bar = |offset: u16, restore: u32| -> u32 {
        let command = bus.command(slot, function);
        if command & 0x03 != 0 {
            bus.set_command(slot, function, command & !0x03);
        }

        let mask = unsafe {
            bus.set_bar(slot, function, offset, 0xFFFFFFFF);
            let mask = bus.bar(slot, function, offset);
            bus.set_bar(slot, function, offset, restore);

            mask
        };

        if command & 0x03 != 0 {
            bus.set_command(slot, function, command);
        }

        mask
    };

    let mut i = 0;
    while i < n_bars {
        let offset = PCI_REGULAR_BAR0 + (i as u16) * 4;
        let bar = unsafe { bus.bar(slot, function, offset) };

        if bar & 1 != 0 {
            let address = (bar & 0xFFFFFFFC) as u64;
            let mask = size_bar(offset, bar) & 0xFFFFFFFC;

            // The device does not decode any address bits from this BAR.
            if mask == 0 {
                i += 1;
                continue;
            }

            let length = compute_bar_length(mask as u64);

            bars[i].type_ = BarType::Io;
            bars[i].address = address;
            bars[i].length = length;

            if address == 0 {
                println!("sif:     unallocated I/O space BAR #{i}, length: {length} ports");
            } else {
                // Check the parent resources to see if this BAR is actually memory mapped.
                let mut host = None;
                for res in bus.resources.lock().expect(EXPECT_LOCK).iter() {
                    if res.flags() == PciBusResource::IO
                        && address >= res.base()
                        && address + length <= res.base() + res.size()
                    {
                        host = Some((res.host_base() + (address - res.base()), res.is_host_mmio()));
                        break;
                    }
                }

                if let Some((host_address, true)) = host {
                    bars[i].host_type = BarType::Memory;
                    bars[i].allocated = true;
                    bars[i].host_address = host_address;
                    bars[i].offset = (host_address as usize & PAGE_MASK) as u32;
                } else {
                    bars[i].host_type = BarType::Io;
                    bars[i].allocated = true;
                    bars[i].host_address = address;
                    bars[i].offset = 0;
                }

                println!("sif:     I/O space BAR #{i} at {address:#x}, length: {length} ports");
            }

            i += 1;
        } else if (bar >> 1) & 3 == 0 {
            let address = (bar & 0xFFFFFFF0) as u64;
            let mask = size_bar(offset, bar) & 0xFFFFFFF0;

            // The device does not decode any address bits from this BAR.
            if mask == 0 {
                i += 1;
                continue;
            }

            let length = compute_bar_length(mask as u64);
            let prefetchable = bar & (1 << 3) != 0;

            bars[i].type_ = BarType::Memory;
            bars[i].address = address;
            bars[i].length = length;
            bars[i].prefetchable = prefetchable;

            if address == 0 {
                println!(
                    "sif:     unallocated 32-bit memory BAR #{i}, length: {length} bytes{}",
                    if prefetchable { " (prefetchable)" } else { "" }
                );
            } else {
                bars[i].host_type = BarType::Memory;
                bars[i].allocated = true;
                bars[i].host_address = address;
                bars[i].offset = (address as usize & PAGE_MASK) as u32;

                println!(
                    "sif:     32-bit memory BAR #{i} at {address:#x}, length: {length} bytes{}",
                    if prefetchable { " (prefetchable)" } else { "" }
                );
            }

            i += 1;
        } else if (bar >> 1) & 3 == 2 {
            if i + 1 >= n_bars {
                return Err(PciError::TruncatedBar64 { index: i });
            }
            let high = unsafe { bus.bar(slot, function, offset + 4) };
            let address = ((high as u64) << 32) | (bar & 0xFFFFFFF0) as u64;

            let command = bus.command(slot, function);
            if command & 0x03 != 0 {
                bus.set_command(slot, function, command & !0x03);
            }

            let mask = unsafe {
                bus.set_bar(slot, function, offset, 0xFFFFFFFF);
                bus.set_bar(slot, function, offset + 4, 0xFFFFFFFF);
                let mask = ((bus.bar(slot, function, offset + 4) as u64) << 32)
                    | (bus.bar(slot, function, offset) & 0xFFFFFFF0) as u64;
                bus.set_bar(slot, function, offset, bar);
                bus.set_bar(slot, function, offset + 4, high);

                mask
            };

            if command & 0x03 != 0 {
                bus.set_command(slot, function, command);
            }

            // The device does not decode any address bits from this BAR.
            if mask == 0 {
                i += 2;
                continue;
            }

            let length = compute_bar_length(mask);
            let prefetchable = bar & (1 << 3) != 0;

            bars[i].type_ = BarType::Memory;
            bars[i].address = address;
            bars[i].length = length;
            bars[i].prefetchable = prefetchable;

            if address == 0 {
                println!(
                    "sif:     unallocated 64-bit memory BAR #{i}, length: {length} bytes{}",
                    if prefetchable { " (prefetchable)" } else { "" }
                );
            } else {
                bars[i].host_type = BarType::Memory;
                bars[i].allocated = true;
                bars[i].host_address = address;
                bars[i].offset = (address as usize & PAGE_MASK) as u32;

                println!(
                    "sif:     64-bit memory BAR #{i} at {address:#x}, length: {length} bytes{}",
                    if prefetchable { " (prefetchable)" } else { "" }
                );
            }

            i += 2;
        } else {
            return Err(PciError::ReservedBarType { index: i });
        }
    }

    // thor reports CPU physical addresses, hence match against the host side of the BARs.
    for &device in kernel_device::all() {
        let contained = bars.iter().position(|bar| {
            bar.allocated
                && device.resources.iter().any(|resource| {
                    bar.host_type == bar_type_of(resource.kind)
                        && range_contains(bar.host_address, bar.length, resource)
                })
        });
        if let Some(i) = contained {
            println!("sif:     BAR #{i} contains {}", device.describe());
            device.attribute_to(entity);
        }
    }

    Ok(())
}

fn parse_expansion_rom(entity: &PciEntity, offset: u16) {
    let bus = entity.parent_bus;
    let slot = entity.slot;
    let function = entity.function;

    // The caller passes the expansion ROM offset of the header type of the entity.
    let expansion_rom_addr = unsafe { bus.expansion_rom_base(slot, function, offset) };
    // Write all 1s to the expansion ROM address and read it back to determine its length.
    let expansion_rom_mask = unsafe {
        bus.set_expansion_rom_base(slot, function, offset, 0xFFFFFFFF);

        let mask = bus.expansion_rom_base(slot, function, offset) & 0xFFFFFFFC;
        bus.set_expansion_rom_base(slot, function, offset, expansion_rom_addr);

        mask
    };

    if expansion_rom_mask == 0 {
        return;
    }

    let expansion_rom_length = compute_bar_length(expansion_rom_mask as u64);
    // Enable it.
    unsafe { bus.set_expansion_rom_base(slot, function, offset, expansion_rom_addr | 1) };

    let rom = PciExpansionRom {
        address: expansion_rom_addr as u64,
        length: expansion_rom_length,
    };
    assert!(
        entity.expansion_rom.set(rom).is_ok(),
        "sif: PCI expansion ROM was already sized"
    );
}

fn find_pci_caps(entity: &PciEntity) -> Result<(), PciError> {
    let bus = entity.parent_bus;
    let slot = entity.slot;
    let function = entity.function;

    let status = bus.status(slot, function);

    // Find all capabilities.
    if status & 0x10 != 0 {
        // The bottom two bits of each capability offset must be masked!
        let mut offset = (bus.capabilities_pointer(slot, function) & 0xFC) as u16;
        let mut remaining = MAX_CAPABILITIES;
        while offset != 0 {
            if remaining == 0 {
                return Err(PciError::CapabilityLoop);
            }
            remaining -= 1;

            // Capability headers sit at device-defined offsets, hence we cannot use a
            // safe accessor to read them.
            let ent = unsafe { bus.read_config_half(slot, function, offset) };
            let type_ = (ent & 0xFF) as u32;

            if let Some(name) = name_of_capability(type_) {
                println!("sif:     {name} capability");
            } else {
                println!("sif:     Capability of type {type_:#04x}");
            }

            if type_ == 0x10 {
                entity.is_pcie.store(true, Ordering::Relaxed);

                let flags = unsafe { bus.read_config_half(slot, function, offset + 2) };
                entity
                    .pcie_port_type
                    .store(((flags >> 4) & 0xF) as u8, Ordering::Relaxed);
            }

            let length = if type_ == 0x09 {
                Some(unsafe { bus.read_config_byte(slot, function, offset + 2) } as u64)
            } else {
                None
            };

            entity.caps.lock().expect(EXPECT_LOCK).push(Capability {
                type_,
                offset,
                length,
            });

            offset = ((ent >> 8) & 0xFC) as u16;
        }
    }

    // PCIe devices are required to provide the 4096-byte configuration space.
    if bus.io.supports_4k_config_space() && entity.is_pcie.load(Ordering::Relaxed) {
        let mut offset: u16 = 0x100;
        let mut remaining = MAX_EXTENDED_CAPABILITIES;

        while offset != 0 {
            if remaining == 0 {
                return Err(PciError::ExtendedCapabilityLoop);
            }
            remaining -= 1;

            // Extended capability headers sit at device-defined offsets, hence we cannot
            // use a safe accessor to read them.
            let data = unsafe { bus.read_config_word(slot, function, offset) };
            let extended_cap_id = (data & 0xFFFF) as u16;
            let version = (data >> 16) & 0xF;
            // The bottom 2 bits are reserved and must be masked out.
            // This offset is relative to the start of the entire configuration space.
            let next_offset = ((data >> 20) & 0xFFC) as u16;
            if next_offset > 0 && next_offset < 0x100 {
                println!("sif: invalid 'Next Capability Offset' {next_offset:#x}, skipping");
                break;
            }

            // Any one of these conditions signals that there are no further extended capabilities.
            if extended_cap_id == 0xFFFF || extended_cap_id == 0 || next_offset == 0 {
                break;
            }

            // We have a valid extended capability.
            if let Some(name) = name_of_extended_capability(extended_cap_id) {
                println!("sif:     {name} Extended Capability (v{version})");
            } else {
                println!("sif:     Extended Capability {extended_cap_id:#x} (v{version})");
            }

            entity
                .extended_caps
                .lock()
                .expect(EXPECT_LOCK)
                .push(ExtendedCapability {
                    type_: extended_cap_id,
                    offset,
                });

            offset = next_offset;
        }
    }

    Ok(())
}

// Setup MSI / MSI-X. This needs to happen after BARs are already allocated.
fn setup_msis(device: &'static PciDevice) -> Result<(), PciError> {
    let entity = &device.entity;
    let bus = entity.parent_bus;
    let slot = entity.slot;
    let function = entity.function;

    let msix_index = device.msix_index.load(Ordering::Relaxed);
    let msi_index = device.msi_index.load(Ordering::Relaxed);
    if msix_index >= 0 {
        let offset = entity.caps.lock().expect(EXPECT_LOCK)[msix_index as usize].offset;

        let msg_control = unsafe { bus.read_config_half(slot, function, offset + 2) };
        let num_msis = ((msg_control & 0x7FF) + 1) as u32;
        println!(
            "sif: {num_msis} MSI-X vectors available for PCI device {:04x}:{:02x}:{:02x}.{:x}",
            entity.seg, entity.bus, slot, function
        );

        // Map the MSI-X BAR.
        let table_info = unsafe { bus.read_config_word(slot, function, offset + 4) };
        let table_bar = (table_info & 0x7) as usize;
        let table_offset = table_info & 0xFFFF_FFF8;

        let bar = entity
            .bars
            .lock()
            .expect(EXPECT_LOCK)
            .get(table_bar)
            .copied()
            .filter(|bar| bar.type_ == BarType::Memory)
            .ok_or(PciError::MsixTableNotInMemoryBar { bir: table_bar })?;
        if !bar.allocated {
            return Err(PciError::MsixTableBarUnallocated { bir: table_bar });
        }
        if table_offset as u64 + num_msis as u64 * 16 > bar.length {
            return Err(PciError::MsixTableOutsideBar { bir: table_bar });
        }
        let table_address = (bar.host_address + table_offset as u64) as usize;
        let mapping_disp = table_address & PAGE_MASK;
        let mapping_size = (mapping_disp + num_msis as usize * 16 + PAGE_MASK) & !PAGE_MASK;

        let handle = hel::access_physical(
            hardware_access_handle(),
            table_address & !PAGE_MASK,
            mapping_size,
            hel::CachingMode::Mmio,
        )?;
        let mapping = unsafe {
            hel::Mapping::<u8>::new(
                &handle,
                None,
                0,
                mapping_size,
                hel::MappingFlags::READ | hel::MappingFlags::WRITE,
            )
        }?;
        let table = MsixTable::new(mapping, mapping_disp);

        // Mask all MSIs.
        for i in 0..num_msis as usize {
            table.set_masked(i, true);
        }

        *device.msix_mapping.lock().expect(EXPECT_LOCK) = Some(table);
        device.num_msis.store(num_msis, Ordering::Relaxed);
        device.msis_usable.store(true, Ordering::Relaxed);
    } else if msi_index >= 0 {
        let offset = entity.caps.lock().expect(EXPECT_LOCK)[msi_index as usize].offset;

        let mut msg_control = unsafe { bus.read_config_half(slot, function, offset + 2) };
        let num_msis = 1; // TODO(qookie): 1 << ((msg_control >> 1) & 0b111)
        println!(
            "sif: {num_msis} MSI vectors available for PCI device {:04x}:{:02x}:{:02x}.{:x}",
            entity.seg, entity.bus, slot, function
        );

        msg_control &= !0x0001; // Disable MSI

        unsafe { bus.write_config_half(slot, function, offset + 2, msg_control) };
        device.num_msis.store(num_msis, Ordering::Relaxed);
        device.msis_usable.store(true, Ordering::Relaxed);
    }

    Ok(())
}

fn check_pci_function(
    bus: &'static PciBus,
    slot: u8,
    function: u8,
    enumerate_downstream: &mut dyn FnMut(&'static PciBus) -> Result<(), PciError>,
) -> Result<(), PciError> {
    let vendor = bus.vendor(slot, function);
    if vendor == 0xFFFF {
        return Ok(());
    }

    let header_type = bus.header_type(slot, function);
    if header_type & 0x7F == 0 {
        println!("sif:   Function {function}: Device");
    } else if header_type & 0x7F == 1 {
        let downstream_id = unsafe { bus.secondary_bus(slot, function) };

        if downstream_id == 0 {
            println!("sif:   Function {function}: unconfigured PCI-to-PCI bridge");
        } else {
            println!("sif:   Function {function}: PCI-to-PCI bridge to bus {downstream_id}");
        }
    } else {
        println!(
            "sif:   Function {function}: Unexpected PCI header type {}",
            header_type & 0x7F
        );
    }

    // Disable interrupts until a driver configures the device.
    //
    // We don't disable bus mastering since that breaks devices such as integrated GPUs.
    // Linux does not disable bus mastering either and we can rely on IOMMUs for DMA protection.
    // I/O and memory decoding is only disabled while we size BARs.
    let mut command = bus.command(slot, function);
    command |= 0x400; // Mask IRQs.
    bus.set_command(slot, function, command);

    let device_id = bus.device_id(slot, function);
    let revision = bus.revision(slot, function);
    let class_code = bus.class_code(slot, function);
    let sub_class = bus.sub_class(slot, function);
    let interface = bus.interface(slot, function);

    println!(
        "sif:     Vendor/device: {vendor:04x}.{device_id:04x}.{revision:02x}, \
                class: {class_code:02x}.{sub_class:02x}.{interface:02x}"
    );

    if header_type & 0x7F == 0 {
        let subsystem_vendor = unsafe { bus.subsystem_vendor(slot, function) };
        let subsystem_device = unsafe { bus.subsystem_device(slot, function) };

        let status = bus.status(slot, function);
        if status & 0x08 != 0 {
            println!("sif:       IRQ is asserted!");
        }

        let device = PciDevice::new(
            bus,
            slot,
            function,
            vendor,
            device_id,
            revision,
            class_code,
            sub_class,
            interface,
            subsystem_vendor,
            subsystem_device,
        );

        find_pci_caps(&device.entity)?;

        for (i, cap) in device
            .entity
            .caps
            .lock()
            .expect(EXPECT_LOCK)
            .iter()
            .enumerate()
        {
            if cap.type_ == 0x5 {
                device.msi_index.store(i as i32, Ordering::Relaxed);
            }
            if cap.type_ == 0x11 {
                device.msix_index.store(i as i32, Ordering::Relaxed);
            }
        }

        read_entity_bars(&device.entity, 6)?;
        parse_expansion_rom(&device.entity, PCI_REGULAR_EXPANSION_ROM_BASE_ADDRESS);

        if let Some(irq_index) = IrqIndex::from_pin(bus.interrupt_pin(slot, function)) {
            let router = bus.irq_router.get().expect("bus has no IRQ router");
            if let Some(pin) = router.resolve_irq_route(slot, irq_index) {
                println!(
                    "sif:     Interrupt: {} (routed to {})",
                    irq_index.name(),
                    pin.name()
                );
                assert!(
                    device.interrupt.set(pin).is_ok(),
                    "sif: PCI device was already enumerated"
                );
            } else {
                println!("sif:     Interrupt routing not available!");
            }
        }

        ALL_DEVICES.lock().expect(EXPECT_LOCK).push(device);
        bus.child_devices.lock().expect(EXPECT_LOCK).push(device);
    } else if header_type & 0x7F == 1 {
        let bridge = PciBridge::new(
            bus, slot, function, vendor, device_id, revision, class_code, sub_class, interface,
        );

        find_pci_caps(&bridge.entity)?;

        read_entity_bars(&bridge.entity, 2)?;
        parse_expansion_rom(&bridge.entity, PCI_BRIDGE_EXPANSION_ROM_BASE_ADDRESS);

        let downstream_id = unsafe { bus.secondary_bus(slot, function) };

        if downstream_id != 0 {
            bridge.downstream_id.store(downstream_id, Ordering::Relaxed);
            bridge.subordinate_id.store(
                unsafe { bus.subordinate_bus(slot, function) },
                Ordering::Relaxed,
            );

            let downstream_bus = bus.make_downstream_bus(bridge, downstream_id);
            assert!(
                bridge.associated_bus.set(downstream_bus).is_ok(),
                "sif: PCI bridge was already enumerated"
            );
            enumerate_downstream(downstream_bus)?;
        } else {
            println!("sif:     Deferring enumeration until the bridge is configured");
        }

        // Only register the bridge once nothing can fail anymore.
        bus.child_bridges.lock().expect(EXPECT_LOCK).push(bridge);
    }

    Ok(())
}

fn check_pci_device(
    bus: &'static PciBus,
    slot: u8,
    enumerate_downstream: &mut dyn FnMut(&'static PciBus) -> Result<(), PciError>,
) {
    let vendor = bus.vendor(slot, 0);
    if vendor == 0xFFFF {
        return;
    }

    println!(
        "sif: Segment: {}, bus: {}, slot {}",
        bus.seg_id, bus.bus_id, slot
    );

    let header_type = bus.header_type(slot, 0);
    let functions = if header_type & 0x80 != 0 { 0..8 } else { 0..1 };
    for function in functions {
        if let Err(err) = check_pci_function(bus, slot, function, enumerate_downstream) {
            println!(
                "sif: Ignoring PCI function {:04x}:{:02x}:{:02x}.{function}: {err}",
                bus.seg_id, bus.bus_id, slot
            );
            quiesce_function(bus, slot, function);
        }
    }
}

// sif does not track ignored functions, hence they must not decode, forward or DMA anything.
fn quiesce_function(bus: &'static PciBus, slot: u8, function: u8) {
    let command = bus.command(slot, function);
    bus.set_command(slot, function, command & !0x07);

    if bus.header_type(slot, function) & 0x7F == 1 {
        unsafe {
            bus.set_secondary_bus(slot, function, 0);
            bus.set_subordinate_bus(slot, function, 0);
        }
    }
}

fn check_pci_bus(
    bus: &'static PciBus,
    enumerate_downstream: &mut dyn FnMut(&'static PciBus) -> Result<(), PciError>,
) {
    let bridge = bus.associated_bridge;
    let mut n_slots: u8 = 32;

    // A PCIe downstream port has only one device (slot 0) attached.
    // In theory, this is only an optimization, in practice however omitting
    // this causes a SError on the BCM2711 when trying to access the vendor ID
    // of a non-existant device.
    if let Some(bridge) = bridge
        && bridge.entity.is_downstream_port()
    {
        n_slots = 1;
    }

    for slot in 0..n_slots {
        check_pci_device(bus, slot, enumerate_downstream);
    }
}

fn check_for_bridge_resources(bridge: &'static PciBridge) {
    let parent_bus = bridge.entity.parent_bus;
    let slot = bridge.entity.slot;
    let function = bridge.entity.function;
    let downstream = bridge
        .associated_bus
        .get()
        .expect("bridge has no associated bus");

    {
        // The low nibble of the I/O base/limit encodes capabilities, not address bits.
        let base = (unsafe { parent_bus.io_base(slot, function) } & 0xF0) as u64;
        let limit = (unsafe { parent_bus.io_limit(slot, function) } & 0xF0) as u64;

        let addr = base << 8;
        let size = (limit << 8).wrapping_add(0x100).wrapping_sub(addr);

        // Try to look up the host base address in our parent's resources.
        let mut host_base = 0;
        let mut is_host_mmio = false;
        for res in parent_bus.resources.lock().expect(EXPECT_LOCK).iter() {
            if res.base() <= addr
                && res.base() + res.size() >= addr.wrapping_add(size)
                && res.flags() == PciBusResource::IO
            {
                is_host_mmio = res.is_host_mmio();
                host_base = res.host_base() + (addr - res.base());
                break;
            }
        }

        // If the window is not found in the parent's resources, assume it's
        // garbage/empty and ignore it.
        if host_base != 0 && size != 0 {
            println!(
                "sif: Discovered existing I/O window of bridge \
                        {:04x}:{:02x}:{:02x}.{} address: {addr:#x} size: {size} \
                        (host base: {host_base:#x})",
                bridge.entity.seg, bridge.entity.bus, slot, function
            );

            downstream
                .resources
                .lock()
                .expect(EXPECT_LOCK)
                .push(PciBusResource::new(
                    addr,
                    size,
                    host_base,
                    PciBusResource::IO,
                    is_host_mmio,
                ));
        }
    }

    {
        let base = unsafe { parent_bus.mem_base(slot, function) } as u64;
        let limit = unsafe { parent_bus.mem_limit(slot, function) } as u64;

        let addr = base << 16;
        let size = (limit << 16).wrapping_add(0x100000).wrapping_sub(addr);

        // Try to look up the host base address in our parent's resources.
        let mut host_base = 0;
        for res in parent_bus.resources.lock().expect(EXPECT_LOCK).iter() {
            if res.base() <= addr
                && res.base() + res.size() >= addr.wrapping_add(size)
                && res.flags() == PciBusResource::MEMORY
            {
                host_base = res.host_base() + (addr - res.base());
                break;
            }
        }

        // If the window is not found in the parent's resources, assume it's
        // garbage/empty and ignore it.
        if host_base != 0 && size != 0 {
            println!(
                "sif: Discovered existing memory window of bridge \
                        {:04x}:{:02x}:{:02x}.{} address: {addr:#x} size: {size} \
                        (host base: {host_base:#x})",
                bridge.entity.seg, bridge.entity.bus, slot, function
            );

            downstream
                .resources
                .lock()
                .expect(EXPECT_LOCK)
                .push(PciBusResource::new(
                    addr,
                    size,
                    host_base,
                    PciBusResource::MEMORY,
                    true,
                ));
        }
    }

    {
        // The low nibble of the prefetch base/limit encodes 64-bit support, not address bits.
        let base = (unsafe { parent_bus.prefetch_mem_base(slot, function) } & 0xFFF0) as u64;
        let limit = (unsafe { parent_bus.prefetch_mem_limit(slot, function) } & 0xFFF0) as u64;

        let base_upper = unsafe { parent_bus.prefetch_mem_base_upper(slot, function) } as u64;
        let limit_upper = unsafe { parent_bus.prefetch_mem_limit_upper(slot, function) } as u64;

        let addr = (base << 16) | (base_upper << 32);
        let size = ((limit << 16) | (limit_upper << 32))
            .wrapping_add(0x100000)
            .wrapping_sub(addr);

        // Try to look up the host base address in our parent's resources.
        let mut host_base = 0;
        for res in parent_bus.resources.lock().expect(EXPECT_LOCK).iter() {
            if res.base() <= addr
                && res.base() + res.size() >= addr.wrapping_add(size)
                && res.flags() == PciBusResource::PREF_MEMORY
            {
                host_base = res.host_base() + (addr - res.base());
                break;
            }
        }

        // If the window is not found in the parent's resources, assume it's
        // garbage/empty and ignore it.
        if host_base != 0 && size != 0 {
            println!(
                "sif: Discovered existing prefetch memory window of bridge \
                        {:04x}:{:02x}:{:02x}.{} address: {addr:#x} size: {size} \
                        (host base: {host_base:#x})",
                bridge.entity.seg, bridge.entity.bus, slot, function
            );

            downstream
                .resources
                .lock()
                .expect(EXPECT_LOCK)
                .push(PciBusResource::new(
                    addr,
                    size,
                    host_base,
                    PciBusResource::PREF_MEMORY,
                    true,
                ));
        }
    }
}

fn configure_bridges(bus: &'static PciBus, highest_id: &mut u8) {
    let mut i = 0;
    while let Some(bridge) = {
        let bridges = bus.child_bridges.lock().expect(EXPECT_LOCK);
        bridges.get(i).copied()
    } {
        if bridge.downstream_id.load(Ordering::Relaxed) == 0 {
            let parent = bridge.entity.parent_bus.associated_bridge;

            let mut b = parent;
            while let Some(cur) = b {
                let subordinate_id = cur.subordinate_id.load(Ordering::Relaxed);
                println!(
                    "sif: Bumping bridge {:04x}:{:02x}:{:02x}.{} from subordinate id {} \
                            to subordinate id {}",
                    cur.entity.seg,
                    cur.entity.bus,
                    cur.entity.slot,
                    cur.entity.function,
                    subordinate_id,
                    subordinate_id + 1
                );

                cur.subordinate_id
                    .store(subordinate_id + 1, Ordering::Relaxed);
                unsafe {
                    cur.entity.parent_bus.set_subordinate_bus(
                        cur.entity.slot,
                        cur.entity.function,
                        subordinate_id + 1,
                    )
                };
                b = cur.entity.parent_bus.associated_bridge;
            }

            if let Some(parent) = parent {
                let subordinate_id = parent.subordinate_id.load(Ordering::Relaxed);
                assert!(*highest_id < subordinate_id);
                *highest_id = subordinate_id;

                bridge
                    .downstream_id
                    .store(subordinate_id, Ordering::Relaxed);
                bridge
                    .subordinate_id
                    .store(subordinate_id, Ordering::Relaxed);
            } else {
                // We're directly on the root bus.
                // TODO: this ID may be in use by a bridge on a different root bus.
                *highest_id += 1;

                bridge.downstream_id.store(*highest_id, Ordering::Relaxed);
                bridge.subordinate_id.store(*highest_id, Ordering::Relaxed);
            }

            let downstream_id = bridge.downstream_id.load(Ordering::Relaxed);
            let subordinate_id = bridge.subordinate_id.load(Ordering::Relaxed);
            unsafe {
                bridge.entity.parent_bus.set_secondary_bus(
                    bridge.entity.slot,
                    bridge.entity.function,
                    downstream_id,
                );
                bridge.entity.parent_bus.set_subordinate_bus(
                    bridge.entity.slot,
                    bridge.entity.function,
                    subordinate_id,
                );
            }

            println!(
                "sif: Found unconfigured bridge {:04x}:{:02x}:{:02x}.{}, now configured to \
                        downstream {}, subordinate {}",
                bridge.entity.seg,
                bridge.entity.bus,
                bridge.entity.slot,
                bridge.entity.function,
                downstream_id,
                subordinate_id
            );

            let downstream_bus = bus.make_downstream_bus(bridge, downstream_id);
            assert!(
                bridge.associated_bus.set(downstream_bus).is_ok(),
                "sif: PCI bridge was already enumerated"
            );
            check_pci_bus(downstream_bus, &mut |_| {
                Err(PciError::ConfiguredBridgeBelowUnconfigured)
            });
        }

        let downstream_bus = bridge
            .associated_bus
            .get()
            .expect("Bridge has no associated bus");

        // Look for any existing bridge resources.
        check_for_bridge_resources(bridge);

        configure_bridges(downstream_bus, highest_id);

        i += 1;
    }
}

struct SpaceRequirement {
    size: u64,
    flags: u32,

    // For devices (or bridge BARs)
    index: usize,
    associated_entity: Option<&'static PciEntity>,

    // For devices behind this bridge
    associated_bridge: Option<&'static PciBridge>,
}

fn process_bar(required: &mut Vec<SpaceRequirement>, entity: &'static PciEntity, i: usize) {
    let bar = entity.bars.lock().expect(EXPECT_LOCK)[i];

    if bar.allocated {
        return;
    }

    let flags = match bar.type_ {
        BarType::None => 0,
        BarType::Io => PciBusResource::IO,
        BarType::Memory => {
            if bar.prefetchable {
                PciBusResource::PREF_MEMORY
            } else {
                PciBusResource::MEMORY
            }
        }
    };

    if flags != 0 {
        required.push(SpaceRequirement {
            size: bar.length,
            flags,
            index: i,
            associated_entity: Some(entity),
            associated_bridge: None,
        });
    }
}

fn get_required_space_for_bus(bus: &'static PciBus) -> Vec<SpaceRequirement> {
    let mut required = Vec::new();

    for dev in bus.child_devices.lock().expect(EXPECT_LOCK).iter() {
        for i in 0..6 {
            process_bar(&mut required, &dev.entity, i);
        }
    }

    for bridge in bus.child_bridges.lock().expect(EXPECT_LOCK).iter() {
        for i in 0..2 {
            process_bar(&mut required, &bridge.entity, i);
        }

        let downstream = bridge
            .associated_bus
            .get()
            .expect("bridge has no associated bus");

        // Only require memory allocations if this bus doesn't already have any resources.
        if downstream.resources.lock().expect(EXPECT_LOCK).is_empty() {
            // Check requirements below the bridge.
            let bridge_req = get_required_space_for_bus(downstream);

            let mut required_io: u64 = 0;
            let mut required_mem: u64 = 0;
            let mut required_pref_memory: u64 = 0;

            for req in &bridge_req {
                if req.flags == PciBusResource::IO {
                    required_io += req.size;
                } else if req.flags == PciBusResource::PREF_MEMORY {
                    required_pref_memory += req.size;
                } else {
                    assert!(req.flags == PciBusResource::MEMORY);
                    required_mem += req.size;
                }
            }

            if required_io != 0 {
                // I/O decoded by the bridge has 256 byte granularity,
                // but the spec requires it to be 4K aligned.
                required.push(SpaceRequirement {
                    size: (required_io + 0xFFF) & !0xFFF,
                    flags: PciBusResource::IO,
                    index: 0,
                    associated_entity: None,
                    associated_bridge: Some(bridge),
                });
            }

            // Memory decoded by the bridge has 1 MiB granularity.

            if required_mem != 0 {
                required.push(SpaceRequirement {
                    size: (required_mem + 0xFFFFF) & !0xFFFFF,
                    flags: PciBusResource::MEMORY,
                    index: 0,
                    associated_entity: None,
                    associated_bridge: Some(bridge),
                });
            }

            if required_pref_memory != 0 {
                required.push(SpaceRequirement {
                    size: (required_pref_memory + 0xFFFFF) & !0xFFFFF,
                    flags: PciBusResource::PREF_MEMORY,
                    index: 0,
                    associated_entity: None,
                    associated_bridge: Some(bridge),
                });
            }
        }
    }

    // We group the same requirement types and sort them by size in descending
    // order to guarantee best fit allocations for requirements of the same type.
    required.sort_by(|a, b| {
        if a.flags == b.flags {
            b.size.cmp(&a.size)
        } else {
            a.flags.cmp(&b.flags)
        }
    });

    required
}

fn is_preferred(old: Option<&PciBusResource>, new: &PciBusResource) -> bool {
    let Some(old) = old else {
        return true;
    };

    if new.base() > old.base() {
        return true;
    }

    new.remaining() < old.remaining()
}

// On success, returns (child base, host base, resource flags, is host MMIO).
fn allocate_bar(bus: &'static PciBus, size: u64, req_flags: u32) -> Option<(u64, u64, u32, bool)> {
    let mut resources = bus.resources.lock().expect(EXPECT_LOCK);

    let is_addressable = |flags: u32, addr: u64| {
        if flags == PciBusResource::IO {
            return true;
        }

        if flags != PciBusResource::PREF_MEMORY {
            return addr < 0x1_0000_0000;
        }

        true
    };

    let mut best: Option<usize> = None;

    for (idx, res) in resources.iter().enumerate() {
        if (req_flags == PciBusResource::PREF_MEMORY || req_flags == PciBusResource::MEMORY)
            && res.flags() == PciBusResource::IO
        {
            continue;
        }

        if (res.flags() == PciBusResource::PREF_MEMORY || res.flags() == PciBusResource::MEMORY)
            && req_flags == PciBusResource::IO
        {
            continue;
        }

        if req_flags == res.flags() && res.can_fit(size) && is_addressable(req_flags, res.base()) {
            best = Some(idx);
            break;
        }

        if req_flags == PciBusResource::PREF_MEMORY
            && res.flags() != PciBusResource::PREF_MEMORY
            && res.can_fit(size)
            && is_preferred(best.map(|best| &resources[best]), res)
        {
            best = Some(idx);
        }
    }

    let best = best?;
    let res = &mut resources[best];
    let off = res.allocate(size).expect("resource must fit after can_fit");

    Some((
        res.base() + off,
        res.host_base() + off,
        res.flags(),
        res.is_host_mmio(),
    ))
}

fn flags_to_str(flags: u32) -> &'static str {
    if flags == PciBusResource::IO {
        return "I/O";
    }
    if flags == PciBusResource::PREF_MEMORY {
        return "pref memory";
    }
    assert!(flags == PciBusResource::MEMORY);
    "memory"
}

fn allocate_bars(bus: &'static PciBus) {
    let required = get_required_space_for_bus(bus);

    println!(
        "sif: Allocating space for entities on bus {:04x}:{:02x}:",
        bus.seg_id, bus.bus_id
    );

    for req in required {
        let entity: &'static PciEntity = match req.associated_entity {
            Some(entity) => entity,
            None => &req.associated_bridge.unwrap().entity,
        };

        let what = if req.associated_bridge.is_some() {
            "window of bridge".to_string()
        } else {
            format!("BAR #{} of entity", req.index)
        };

        let Some((child_base, host_base, _, is_host_mmio)) = allocate_bar(bus, req.size, req.flags)
        else {
            println!(
                "sif: Failed to allocate {} {what} {:04x}:{:02x}:{:02x}.{}",
                flags_to_str(req.flags),
                entity.seg,
                entity.bus,
                entity.slot,
                entity.function
            );
            continue;
        };

        if let Some(bridge) = req.associated_bridge {
            match req.flags {
                PciBusResource::IO => unsafe {
                    entity.parent_bus.set_io_base(
                        entity.slot,
                        entity.function,
                        (child_base >> 8) as u8,
                    );
                    entity.parent_bus.set_io_limit(
                        entity.slot,
                        entity.function,
                        ((child_base + req.size - 0x100) >> 8) as u8,
                    );
                },
                PciBusResource::MEMORY => unsafe {
                    entity.parent_bus.set_mem_base(
                        entity.slot,
                        entity.function,
                        (child_base >> 16) as u16,
                    );
                    entity.parent_bus.set_mem_limit(
                        entity.slot,
                        entity.function,
                        ((child_base + req.size - 0x100000) >> 16) as u16,
                    );
                },
                PciBusResource::PREF_MEMORY => unsafe {
                    entity.parent_bus.set_prefetch_mem_base(
                        entity.slot,
                        entity.function,
                        (child_base >> 16) as u16,
                    );
                    entity.parent_bus.set_prefetch_mem_limit(
                        entity.slot,
                        entity.function,
                        ((child_base + req.size - 0x100000) >> 16) as u16,
                    );
                    entity.parent_bus.set_prefetch_mem_base_upper(
                        entity.slot,
                        entity.function,
                        (child_base >> 32) as u32,
                    );
                    entity.parent_bus.set_prefetch_mem_limit_upper(
                        entity.slot,
                        entity.function,
                        ((child_base + req.size - 0x100000) >> 32) as u32,
                    );
                },
                _ => panic!("Unhandled PCI bus resource type {}", req.flags),
            }

            bridge
                .associated_bus
                .get()
                .unwrap()
                .resources
                .lock()
                .expect(EXPECT_LOCK)
                .push(PciBusResource::new(
                    child_base,
                    req.size,
                    host_base,
                    req.flags,
                    is_host_mmio,
                ));
        } else {
            let bar_offset = PCI_REGULAR_BAR0 + (req.index as u16) * 4;
            // The requirement was derived from a BAR of the header type of the entity.
            let bar_val = unsafe {
                entity
                    .parent_bus
                    .bar(entity.slot, entity.function, bar_offset)
            };

            // Write the BAR address.
            unsafe {
                entity.parent_bus.set_bar(
                    entity.slot,
                    entity.function,
                    bar_offset,
                    child_base as u32,
                )
            };

            if (bar_val >> 1) & 3 == 2 {
                unsafe {
                    entity.parent_bus.set_bar(
                        entity.slot,
                        entity.function,
                        bar_offset + 4,
                        (child_base >> 32) as u32,
                    )
                };
            }

            // Update our associated BAR object.
            let mut bars = entity.bars.lock().expect(EXPECT_LOCK);
            let bar = &mut bars[req.index];
            bar.allocated = true;
            bar.address = child_base;
            bar.host_type = BarType::Memory;
            bar.host_address = host_base;
            bar.offset = (host_base as usize & PAGE_MASK) as u32;
        }

        let mut line = format!(
            "sif: {} {what} {:04x}:{:02x}:{:02x}.{} allocated to {child_base:#x}",
            flags_to_str(req.flags),
            entity.seg,
            entity.bus,
            entity.slot,
            entity.function
        );
        if child_base != host_base {
            line += &format!(" (host {host_base:#x})");
        }
        println!("{line}, size {} bytes", req.size);
    }

    for bridge in bus.child_bridges.lock().expect(EXPECT_LOCK).iter() {
        allocate_bars(bridge.associated_bus.get().unwrap());
    }
}

fn enable_decodes_in_chain(decode_bits: u16, entity: &'static PciEntity) {
    let mut entity = entity;
    loop {
        let bus = entity.parent_bus;

        let cmd = bus.command(entity.slot, entity.function);
        bus.set_command(entity.slot, entity.function, cmd | decode_bits);

        match bus.associated_bridge {
            Some(bridge) => entity = &bridge.entity,
            None => break,
        }
    }
}

fn configure_device(device: &'static PciDevice) {
    // Enable PIO and/or memory decoding for all devices with PIO and/or memory BARs.
    let mut has_io_bar = false;
    let mut has_memory_bar = false;
    for bar in device.entity.bars.lock().expect(EXPECT_LOCK).iter() {
        if bar.type_ == BarType::Io {
            has_io_bar = true;
        }
        if bar.type_ == BarType::Memory {
            has_memory_bar = true;
        }
    }

    let mut decode_bits: u16 = 0;
    if has_io_bar {
        decode_bits |= 0x01;
    }
    if has_memory_bar {
        decode_bits |= 0x02;
    }
    enable_decodes_in_chain(decode_bits, &device.entity);

    if let Err(err) = setup_msis(device) {
        println!(
            "sif: Not using the MSIs of PCI device {:04x}:{:02x}:{:02x}.{:x}: {err}",
            device.entity.seg, device.entity.bus, device.entity.slot, device.entity.function
        );
    }

    println!(
        "sif: Applying quirks for PCI device {:04x}:{:02x}:{:02x}.{:x}",
        device.entity.seg, device.entity.bus, device.entity.slot, device.entity.function
    );
    super::quirks::apply_pci_device_quirks(device);
}

fn find_highest_id(bus: &'static PciBus) -> u8 {
    let mut id = bus.bus_id;

    for bridge in bus.child_bridges.lock().expect(EXPECT_LOCK).iter() {
        let subordinate_id = bridge.subordinate_id.load(Ordering::Relaxed);
        if subordinate_id == 0 {
            continue;
        }

        if id < subordinate_id {
            id = subordinate_id;
        }
    }

    id
}

pub fn enumerate_all() {
    // Downstream buses are appended as we go, hence this also covers buses behind bridges.
    let mut queue = all_root_buses();
    let mut i = 0;
    while i < queue.len() {
        let bus = queue[i];
        check_pci_bus(bus, &mut |downstream| {
            queue.push(downstream);
            Ok(())
        });
        i += 1;
    }

    // Configure unconfigured bridges.
    println!("sif: Looking for unconfigured PCI bridges");

    for root_bus in all_root_buses() {
        let mut highest_id = find_highest_id(root_bus);
        configure_bridges(root_bus, &mut highest_id);
        allocate_bars(root_bus);
    }

    for device in all_devices() {
        configure_device(device);
    }
}
