use anyhow::{Result, bail};

mod acpi;
mod dt;
mod entity;
mod irq;
// Only x86 has an ISA bus (and hence ISA IRQs).
#[cfg(target_arch = "x86_64")]
mod isa;
mod pci;
mod uacpi;

pub(crate) fn leak<T>(value: T) -> &'static T {
    Box::leak(Box::new(value))
}

fn main() -> Result<()> {
    hel::block_on(async {
        let cmdline = managarm::kerncfg::get_cmdline().await?;
        if !cmdline.split_ascii_whitespace().any(|opt| opt == "sif") {
            println!("sif: disabled on the kernel command line");
            return Ok(());
        }
        println!("sif: enabled");

        let rsdp = managarm::kerncfg::get_acpi_rsdp().await?;
        let (dt_address, dt_size) = managarm::kerncfg::get_device_tree().await?;
        if rsdp == 0 && dt_address == 0 {
            bail!("sif: kernel reported neither an ACPI RSDP nor a device tree");
        }

        // thor publishes dt-node objects whenever a device tree exists, even on ACPI systems.
        if dt_address != 0 {
            dt::node::init(dt_address, dt_size)?;
            dt::irq::init();
        }

        if rsdp != 0 {
            acpi::set_rsdp(rsdp);
            acpi::configure_log_level(&cmdline);
            acpi::uacpi_init()?;

            println!("sif: uACPI initialized");

            // Configure the ISA IRQs before the PCI links to match thor's ordering.
            #[cfg(target_arch = "x86_64")]
            isa::configure_isa_irqs();

            if let Err(err) = acpi::ec::init_events() {
                println!("sif: acpi: failed to initialize EC events: {err}");
            }
        }

        pci::publish_devices().await?;

        println!("sif: published PCI devices");

        if acpi::has_rsdp() {
            acpi::ps2::publish().await?;
            if let Err(err) = acpi::battery::publish().await {
                println!("sif: acpi: failed to publish batteries: {err}");
            }
            if let Err(err) = acpi::usb::publish().await {
                println!("sif: acpi: failed to publish USB controllers: {err}");
            }
        }

        dt::serve::publish_all().await?;

        std::future::pending::<Result<()>>().await
    })?
}
