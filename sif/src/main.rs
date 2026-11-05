use anyhow::{Result, bail};

mod acpi;
mod dt;
mod entity;
mod irq;
// Only x86 has an ISA bus (and hence ISA IRQs).
#[cfg(target_arch = "x86_64")]
mod isa;
mod kernel_device;
mod pci;
mod pio;
mod uacpi;

use uacpi::runtime;

pub(crate) fn leak<T>(value: T) -> &'static T {
    Box::leak(Box::new(value))
}

// Mutexes are only locked for the duration of operations that cannot panic.
pub(crate) const EXPECT_LOCK: &str = "sif: mutex was poisoned";

/// Logs the failure of a subsystem; sif keeps serving the others.
fn degrade(what: &str, result: Result<()>) {
    if let Err(err) = result {
        println!("sif: {what} failed: {err:#}");
    }
}

fn main() -> Result<()> {
    hel::block_on(async {
        let cmdline = managarm::kerncfg::get_cmdline().await?;
        println!("sif: enabled");

        // Enumeration must know which hardware the kernel touches before it reconfigures anything.
        kernel_device::init().await?;

        let rsdp = managarm::kerncfg::get_acpi_rsdp().await?;
        let (dt_address, dt_size) = managarm::kerncfg::get_device_tree().await?;
        if rsdp == 0 && dt_address == 0 {
            bail!("the kernel reported neither an ACPI RSDP nor a device tree");
        }

        // dt-node objects are published whenever a device tree exists, even on ACPI systems.
        if dt_address != 0 {
            degrade("Device tree parsing", dt::node::init(dt_address, dt_size));
            dt::irq::init();
        }

        if rsdp != 0 {
            acpi::set_rsdp(rsdp);
            acpi::configure_log_level(&cmdline);
            runtime::run(acpi::uacpi_init).await?;

            println!("sif: uACPI initialized");

            // Configure the ISA IRQs before the PCI links to match thor's ordering.
            #[cfg(target_arch = "x86_64")]
            isa::configure_isa_irqs();

            degrade("EC event setup", acpi::ec::init_events().await);
            degrade("Power button setup", acpi::pm::init_power_button().await);
        }

        pci::publish_devices().await;
        // Only PCI enumeration blocks on AML.
        runtime::forbid_run_blocking();

        println!("sif: published PCI devices");

        degrade(
            "Freestanding framebuffer publishing",
            kernel_device::publish_freestanding_framebuffers().await,
        );

        if acpi::has_rsdp() {
            degrade("PS/2 publishing", acpi::ps2::publish().await);
            degrade("Battery publishing", acpi::battery::publish().await);
            degrade("PM interface publishing", acpi::pm::publish().await);
        }

        dt::serve::publish_all().await;

        std::future::pending::<Result<()>>().await
    })?
}
