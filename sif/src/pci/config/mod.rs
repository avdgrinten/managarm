pub mod ecam;
pub mod legacy;

use managarm::svrctl::hardware_access_handle;
use std::collections::BTreeMap;
use std::ptr::addr_of;
use std::sync::Mutex;

use anyhow::Context;
use thiserror::Error;

use ecam::EcamPcieConfigIo;
use legacy::LegacyPciConfigIo;

/// Reasons why an access to PCI configuration space can fail.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Error)]
pub enum ConfigIoError {
    #[error("there is no configuration space for {seg:04x}:{bus:02x}")]
    NoConfigSpace { seg: u16, bus: u8 },

    #[error("{seg:04x}:{bus:02x}:{slot:02x}.{function} is not addressable by this backend")]
    InvalidAddress {
        seg: u16,
        bus: u8,
        slot: u8,
        function: u8,
    },

    #[error("register {offset:#x} lies outside of the {limit:#x} byte configuration space")]
    OutOfRange { offset: u16, limit: u16 },

    #[error("register {offset:#x} is not aligned to {size} bytes")]
    Misaligned { offset: u16, size: u8 },

    #[error("failed to map the ECAM window of {seg:04x}:{bus:02x}")]
    MappingFailed {
        seg: u16,
        bus: u8,
        #[source]
        source: hel::Error,
    },
}

pub type Result<T> = std::result::Result<T, ConfigIoError>;

/// Checks that a register of `size` bytes at `offset` fits into a `limit` byte config space.
pub(crate) fn check_offset(offset: u16, size: u8, limit: u16) -> Result<()> {
    if offset % size as u16 != 0 {
        return Err(ConfigIoError::Misaligned { offset, size });
    }
    if offset as usize + size as usize > limit as usize {
        return Err(ConfigIoError::OutOfRange { offset, limit });
    }
    Ok(())
}

/// Raw access to the configuration space of a (segment, bus) pair.
///
/// Since the effect of an access depends on the register that is addressed, all accessors are
/// unsafe; safe accessors for individual registers are built on top of them.
pub trait PciConfigIo: Sync {
    /// # Safety
    ///
    /// See [`PciConfigIo`].
    unsafe fn read_config_byte(
        &self,
        seg: u16,
        bus: u8,
        slot: u8,
        function: u8,
        offset: u16,
    ) -> Result<u8>;

    /// # Safety
    ///
    /// See [`PciConfigIo`].
    unsafe fn read_config_half(
        &self,
        seg: u16,
        bus: u8,
        slot: u8,
        function: u8,
        offset: u16,
    ) -> Result<u16>;

    /// # Safety
    ///
    /// See [`PciConfigIo`].
    unsafe fn read_config_word(
        &self,
        seg: u16,
        bus: u8,
        slot: u8,
        function: u8,
        offset: u16,
    ) -> Result<u32>;

    /// # Safety
    ///
    /// See [`PciConfigIo`].
    unsafe fn write_config_byte(
        &self,
        seg: u16,
        bus: u8,
        slot: u8,
        function: u8,
        offset: u16,
        value: u8,
    ) -> Result<()>;

    /// # Safety
    ///
    /// See [`PciConfigIo`].
    unsafe fn write_config_half(
        &self,
        seg: u16,
        bus: u8,
        slot: u8,
        function: u8,
        offset: u16,
        value: u16,
    ) -> Result<()>;

    /// # Safety
    ///
    /// See [`PciConfigIo`].
    unsafe fn write_config_word(
        &self,
        seg: u16,
        bus: u8,
        slot: u8,
        function: u8,
        offset: u16,
        value: u32,
    ) -> Result<()>;

    fn supports_4k_config_space(&self) -> bool;
}

static CONFIG_SPACES: Mutex<BTreeMap<u32, &'static dyn PciConfigIo>> = Mutex::new(BTreeMap::new());

pub fn add_config_space_io(seg: u16, bus: u8, io: &'static dyn PciConfigIo) {
    CONFIG_SPACES
        .lock()
        .expect("sif: config space registry mutex was poisoned")
        .insert(((seg as u32) << 8) | bus as u32, io);
}

pub fn get_config_io_for(seg: u16, bus: u8) -> Option<&'static dyn PciConfigIo> {
    CONFIG_SPACES
        .lock()
        .expect("sif: config space registry mutex was poisoned")
        .get(&(((seg as u32) << 8) | bus as u32))
        .copied()
}

fn config_io_for(seg: u16, bus: u8) -> Result<&'static dyn PciConfigIo> {
    get_config_io_for(seg, bus).ok_or(ConfigIoError::NoConfigSpace { seg, bus })
}

/// # Safety
///
/// See [`PciConfigIo`].
pub unsafe fn read_config_byte(
    seg: u16,
    bus: u8,
    slot: u8,
    function: u8,
    offset: u16,
) -> Result<u8> {
    let io = config_io_for(seg, bus)?;
    unsafe { io.read_config_byte(seg, bus, slot, function, offset) }
}

/// # Safety
///
/// See [`PciConfigIo`].
pub unsafe fn read_config_half(
    seg: u16,
    bus: u8,
    slot: u8,
    function: u8,
    offset: u16,
) -> Result<u16> {
    let io = config_io_for(seg, bus)?;
    unsafe { io.read_config_half(seg, bus, slot, function, offset) }
}

/// # Safety
///
/// See [`PciConfigIo`].
pub unsafe fn read_config_word(
    seg: u16,
    bus: u8,
    slot: u8,
    function: u8,
    offset: u16,
) -> Result<u32> {
    let io = config_io_for(seg, bus)?;
    unsafe { io.read_config_word(seg, bus, slot, function, offset) }
}

/// # Safety
///
/// See [`PciConfigIo`].
pub unsafe fn write_config_byte(
    seg: u16,
    bus: u8,
    slot: u8,
    function: u8,
    offset: u16,
    value: u8,
) -> Result<()> {
    let io = config_io_for(seg, bus)?;
    unsafe { io.write_config_byte(seg, bus, slot, function, offset, value) }
}

/// # Safety
///
/// See [`PciConfigIo`].
pub unsafe fn write_config_half(
    seg: u16,
    bus: u8,
    slot: u8,
    function: u8,
    offset: u16,
    value: u16,
) -> Result<()> {
    let io = config_io_for(seg, bus)?;
    unsafe { io.write_config_half(seg, bus, slot, function, offset, value) }
}

/// # Safety
///
/// See [`PciConfigIo`].
pub unsafe fn write_config_word(
    seg: u16,
    bus: u8,
    slot: u8,
    function: u8,
    offset: u16,
    value: u32,
) -> Result<()> {
    let io = config_io_for(seg, bus)?;
    unsafe { io.write_config_word(seg, bus, slot, function, offset, value) }
}

fn add_legacy_config_io() -> anyhow::Result<()> {
    // Unlike thor, we need to be granted access to the config window ports.
    let ports: Vec<usize> = (0xCF8..=0xCFF).collect();
    hel::access_io(hardware_access_handle(), &ports)
        .and_then(hel::enable_io)
        .context("failed to enable the legacy PCI config I/O ports")?;

    let io: &'static LegacyPciConfigIo = Box::leak(Box::new(LegacyPciConfigIo::new()));
    for bus in 0..=255u8 {
        add_config_space_io(0, bus, io);
    }
    Ok(())
}

pub fn discover_config_spaces() -> anyhow::Result<()> {
    let mut table = uacpi_sys::uacpi_table::default();
    let status = unsafe { uacpi_sys::uacpi_table_find_by_signature(c"MCFG".as_ptr(), &mut table) };
    if status != uacpi_sys::UACPI_STATUS_OK {
        println!("sif: No MCFG table, assuming legacy PCI");
        return add_legacy_config_io();
    }

    let hdr = unsafe { table.__bindgen_anon_1.hdr };
    if hdr.is_null() {
        unsafe { uacpi_sys::uacpi_table_unref(&mut table) };
        return add_legacy_config_io();
    }
    let mcfg = hdr as *const uacpi_sys::acpi_mcfg;

    let length = unsafe { addr_of!((*mcfg).hdr.length).read_unaligned() } as usize;
    let header_size = size_of::<uacpi_sys::acpi_mcfg>();
    let entry_size = size_of::<uacpi_sys::acpi_mcfg_allocation>();
    let count = length.saturating_sub(header_size) / entry_size;

    let entries =
        unsafe { (mcfg as *const u8).add(header_size) as *const uacpi_sys::acpi_mcfg_allocation };
    struct EcamRegion {
        address: u64,
        segment: u16,
        start_bus: u8,
        end_bus: u8,
    }
    let mut regions = Vec::with_capacity(count);
    for i in 0..count {
        let entry = unsafe { entries.add(i) };
        regions.push(EcamRegion {
            address: unsafe { addr_of!((*entry).address).read_unaligned() },
            segment: unsafe { addr_of!((*entry).segment).read_unaligned() },
            start_bus: unsafe { addr_of!((*entry).start_bus).read_unaligned() },
            end_bus: unsafe { addr_of!((*entry).end_bus).read_unaligned() },
        });
    }

    unsafe { uacpi_sys::uacpi_table_unref(&mut table) };

    if regions.is_empty() {
        println!("sif: MCFG table has no entries, assuming legacy PCI");
        return add_legacy_config_io();
    }

    for region in regions {
        println!(
            "sif: Found config space for segment {}, buses {}-{}, ECAM MMIO base at {:#x}",
            region.segment, region.start_bus, region.end_bus, region.address
        );

        // MCFG base addresses are relative to bus 0, while EcamPcieConfigIo expects
        // a window that starts at the first bus that it decodes.
        let io: &'static EcamPcieConfigIo = Box::leak(Box::new(EcamPcieConfigIo::new(
            region.address + ((region.start_bus as u64) << 20),
            region.segment,
            region.start_bus,
            region.end_bus,
        )));
        for bus in region.start_bus..=region.end_bus {
            add_config_space_io(region.segment, bus, io);
        }
    }

    Ok(())
}
