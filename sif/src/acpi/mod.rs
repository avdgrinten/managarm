pub mod battery;
pub mod dmar;
pub mod ec;
pub mod glue;
pub mod object;
pub mod ps2;

use std::sync::atomic::{AtomicU64, Ordering};

use anyhow::{Result, anyhow};

use uacpi_sys::uacpi_status;

use crate::uacpi::namespace::NamespaceNode;

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

pub fn has_rsdp() -> bool {
    RSDP.load(Ordering::Relaxed) != 0
}

const LOG_LEVELS: &[(&str, uacpi_sys::uacpi_log_level)] = &[
    ("error", uacpi_sys::UACPI_LOG_ERROR),
    ("warn", uacpi_sys::UACPI_LOG_WARN),
    ("info", uacpi_sys::UACPI_LOG_INFO),
    ("trace", uacpi_sys::UACPI_LOG_TRACE),
    ("debug", uacpi_sys::UACPI_LOG_DEBUG),
];

/// Applies the `uacpi.log` kernel command line option.
pub fn configure_log_level(cmdline: &str) {
    let Some(name) = cmdline
        .split_ascii_whitespace()
        .find_map(|opt| opt.strip_prefix("uacpi.log="))
    else {
        return;
    };

    let Some(&(_, level)) = LOG_LEVELS.iter().find(|(candidate, _)| *candidate == name) else {
        println!("sif: ignoring unknown uacpi.log level \"{name}\"");
        return;
    };

    unsafe { uacpi_sys::uacpi_context_set_log_level(level) };
}

/// Evaluates _CCA of a device. ACPI only allows omitting it on x86, where DMA is always
/// coherent.
pub(crate) fn eval_cca(node: NamespaceNode) -> bool {
    match node.eval_simple_integer(c"_CCA") {
        Ok(Some(cca)) => cca != 0,
        Ok(None) => cfg!(target_arch = "x86_64"),
        Err(err) => {
            println!("sif: Failed to evaluate _CCA: {err}");
            false
        }
    }
}

fn check(status: uacpi_status, what: &str) -> Result<()> {
    if status == uacpi_sys::UACPI_STATUS_OK {
        Ok(())
    } else {
        Err(anyhow!("{what} failed: uacpi status {status:?}"))
    }
}

/// The _OSI feature strings that we answer to, which are the ones that Linux advertises.
///
/// Platform AML picks its code path based on these and firmware is only ever tested against
/// the set that Linux answers to.
const HOST_INTERFACES: &[uacpi_sys::uacpi_host_interface] = &[
    // The OS understands ACPI0004 module devices, i.e., containers of other devices.
    uacpi_sys::UACPI_HOST_INTERFACE_MODULE_DEVICE,
    // Processors may be described as ACPI0007 devices instead of Processor() objects.
    uacpi_sys::UACPI_HOST_INTERFACE_PROCESSOR_DEVICE,
    // The platform may ask for idle CPUs through an ACPI000C processor aggregator.
    uacpi_sys::UACPI_HOST_INTERFACE_PROCESSOR_AGGREGATOR_DEVICE,
];

pub fn uacpi_init() -> Result<()> {
    unsafe {
        check(uacpi_sys::uacpi_initialize(0), "uacpi_initialize")?;
        for &interface in HOST_INTERFACES {
            check(
                uacpi_sys::uacpi_enable_host_interface(interface),
                "uacpi_enable_host_interface",
            )?;
        }
        crate::pci::config::discover_config_spaces()?;
        check(uacpi_sys::uacpi_namespace_load(), "uacpi_namespace_load")?;
        check(
            uacpi_sys::uacpi_set_interrupt_model(INTERRUPT_MODEL),
            "uacpi_set_interrupt_model",
        )?;
        if let Err(err) = ec::init() {
            println!("sif: acpi: failed to initialize the EC: {err}");
        }
        check(
            uacpi_sys::uacpi_namespace_initialize(),
            "uacpi_namespace_initialize",
        )?;
    }

    Ok(())
}
