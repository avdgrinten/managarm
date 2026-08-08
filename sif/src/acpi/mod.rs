pub mod glue;

use std::sync::atomic::{AtomicU64, Ordering};

use anyhow::{Result, anyhow};

use uacpi_sys::uacpi_status;

pub(crate) const PAGE_SIZE: usize = 0x1000;
pub(crate) const PAGE_MASK: usize = PAGE_SIZE - 1;

pub(crate) static RSDP: AtomicU64 = AtomicU64::new(0);

#[cfg(target_arch = "x86_64")]
const INTERRUPT_MODEL: uacpi_sys::uacpi_interrupt_model = uacpi_sys::UACPI_INTERRUPT_MODEL_IOAPIC;
#[cfg(target_arch = "aarch64")]
const INTERRUPT_MODEL: uacpi_sys::uacpi_interrupt_model = uacpi_sys::UACPI_INTERRUPT_MODEL_GIC;
#[cfg(target_arch = "riscv64")]
const INTERRUPT_MODEL: uacpi_sys::uacpi_interrupt_model = uacpi_sys::UACPI_INTERRUPT_MODEL_RINTC;

pub fn set_rsdp(addr: u64) {
    RSDP.store(addr, Ordering::Relaxed);
}

fn check(status: uacpi_status, what: &str) -> Result<()> {
    if status == uacpi_sys::UACPI_STATUS_OK {
        Ok(())
    } else {
        Err(anyhow!("{what} failed: uacpi status {status:?}"))
    }
}

pub fn uacpi_init() -> Result<()> {
    unsafe {
        check(uacpi_sys::uacpi_initialize(0), "uacpi_initialize")?;
        crate::pci::config::discover_config_spaces()?;
        check(uacpi_sys::uacpi_namespace_load(), "uacpi_namespace_load")?;
        check(
            uacpi_sys::uacpi_set_interrupt_model(INTERRUPT_MODEL),
            "uacpi_set_interrupt_model",
        )?;
        check(
            uacpi_sys::uacpi_namespace_initialize(),
            "uacpi_namespace_initialize",
        )?;
    }

    Ok(())
}
