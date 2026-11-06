//! The devices that the kernel drives itself (the boot framebuffer and UART), as published
//! by thor's KernelDevice registry.
//!
//! sif never touches these devices, but it reconfigures the buses they sit on:
//! sizing a BAR disables the entity's decoding, and a driver that claims the entity takes it over.
//! thor does not know how its devices are attached, hence sif matches the kernel's resources
//! against BARs and bridge windows and suspends, resumes or claims the kernel device.

use std::collections::HashMap;
use std::sync::{Arc, OnceLock};

use anyhow::Result;
use managarm::hw::Error as HwError;
use managarm::hw::server::{FbInfo, Framebuffer, serve_framebuffer};
use managarm::kerncfg::{self, KernelDeviceResource, KernelDeviceResourceKind, KernelFramebuffer};
use managarm::mbus::{EntityManager, create_entity};
use managarm::svrctl::hardware_access_handle;

use crate::acpi::{PAGE_MASK, PAGE_SIZE};
use crate::entity::{serve_entity_lanes, string};
use crate::pci::PciEntity;
use crate::{EXPECT_LOCK, leak};

pub struct KernelDevice {
    pub id: u64,
    pub name: String,
    pub resources: Vec<KernelDeviceResource>,
    pub framebuffer: Option<KernelFramebuffer>,
    // The PCI entity whose BAR contains the device, once enumeration found one.
    owner: OnceLock<&'static PciEntity>,
}

impl KernelDevice {
    pub fn describe(&self) -> String {
        format!("kernel device {} ({})", self.id, self.name)
    }

    pub fn owner(&self) -> Option<&'static PciEntity> {
        self.owner.get().copied()
    }

    /// Records that the entity's BAR contains the device; claiming the entity claims it too.
    pub fn attribute_to(&'static self, entity: &'static PciEntity) {
        if self.owner.set(entity).is_err() {
            println!("sif: {} is contained in more than one BAR", self.describe());
            return;
        }
        entity.kernel_devices.lock().expect(EXPECT_LOCK).push(self);
    }

    // Failures are logged only: proceeding without the kernel's cooperation is no worse
    // than the state before the registry existed.
    pub async fn suspend(&self) {
        if let Err(err) = kerncfg::suspend_kernel_device(self.id).await {
            println!("sif: Failed to suspend {}: {err}", self.describe());
        }
    }

    pub async fn resume(&self) {
        if let Err(err) = kerncfg::resume_kernel_device(self.id).await {
            println!("sif: Failed to resume {}: {err}", self.describe());
        }
    }

    pub async fn claim(&self) {
        if let Err(err) = kerncfg::claim_kernel_device(self.id).await {
            println!("sif: Failed to claim {}: {err}", self.describe());
        }
    }

    pub fn fb_info(&self) -> Option<FbInfo> {
        self.framebuffer.map(|fb| FbInfo {
            pitch: fb.pitch,
            width: fb.width,
            height: fb.height,
            bpp: fb.bpp,
            fb_type: fb.fb_type,
        })
    }

    /// The framebuffer's memory, mapped write-combining like thor maps it.
    pub fn access_fb_memory(&self) -> managarm::hw::Result<hel::Handle> {
        if self.framebuffer.is_none() {
            return Err(HwError::IllegalArguments);
        }
        let resource = self
            .resources
            .iter()
            .find(|resource| resource.kind == KernelDeviceResourceKind::Mmio)
            .ok_or(HwError::IllegalArguments)?;

        let span = ((resource.length as usize) + PAGE_MASK) & !PAGE_MASK;
        Ok(hel::access_physical(
            hardware_access_handle(),
            resource.base as usize,
            span.max(PAGE_SIZE),
            hel::CachingMode::WriteCombine,
        )?)
    }
}

static DEVICES: OnceLock<Vec<&'static KernelDevice>> = OnceLock::new();

// The hw protocol cannot convey a sub-page offset, hence only page-aligned framebuffers
// can be handed out.
fn usable_framebuffer(device: &kerncfg::KernelDevice) -> Option<KernelFramebuffer> {
    let framebuffer = device.framebuffer?;
    let aligned = device
        .resources
        .iter()
        .find(|resource| resource.kind == KernelDeviceResourceKind::Mmio)
        .is_some_and(|resource| resource.base as usize & PAGE_MASK == 0);
    if !aligned {
        println!(
            "sif: Ignoring framebuffer of kernel device {} ({}) since it is not page-aligned",
            device.id, device.name
        );
        return None;
    }
    Some(framebuffer)
}

pub async fn init() -> Result<()> {
    let devices: Vec<&'static KernelDevice> = kerncfg::list_kernel_devices()
        .await?
        .into_iter()
        .map(|device| {
            let framebuffer = usable_framebuffer(&device);
            leak(KernelDevice {
                id: device.id,
                name: device.name,
                resources: device.resources,
                framebuffer,
                owner: OnceLock::new(),
            })
        })
        .collect();

    for device in &devices {
        for resource in &device.resources {
            let kind = match resource.kind {
                KernelDeviceResourceKind::Mmio => "memory",
                KernelDeviceResourceKind::Io => "I/O ports",
            };
            println!(
                "sif: {} uses {kind} at {:#x}, length {:#x}",
                device.describe(),
                resource.base,
                resource.length
            );
        }
    }

    assert!(
        DEVICES.set(devices).is_ok(),
        "sif: kernel devices were already listed"
    );
    Ok(())
}

pub fn all() -> &'static [&'static KernelDevice] {
    DEVICES.get().map(Vec::as_slice).unwrap_or(&[])
}

/// The framebuffer that enumeration attributed to the entity, if any.
pub fn framebuffer_of(entity: &PciEntity) -> Option<&'static KernelDevice> {
    entity
        .kernel_devices
        .lock()
        .expect(EXPECT_LOCK)
        .iter()
        .copied()
        .find(|device| device.framebuffer.is_some())
}

struct FreestandingFramebuffer(&'static KernelDevice);

impl Framebuffer for FreestandingFramebuffer {
    fn fb_info(&self) -> FbInfo {
        self.0
            .fb_info()
            .expect("freestanding framebuffer has no geometry")
    }

    fn access_fb_memory(&self) -> managarm::hw::Result<hel::Handle> {
        self.0.access_fb_memory()
    }

    async fn claim_device(&self) -> managarm::hw::Result<()> {
        self.0.claim().await;
        Ok(())
    }
}

async fn serve_freestanding(manager: &'static EntityManager, device: &'static KernelDevice) {
    let fb = Arc::new(FreestandingFramebuffer(device));

    serve_entity_lanes(manager, move |lane| {
        hel::spawn(serve_framebuffer(lane, fb.clone()));
    })
    .await;
}

/// Publishes the kernel framebuffers that no PCI entity contains, the way thor publishes
/// them when it enumerates PCI itself.
pub async fn publish_freestanding_framebuffers() -> Result<()> {
    for &device in all() {
        if device.framebuffer.is_none() || device.owner().is_some() {
            continue;
        }

        let mut props = HashMap::new();
        props.insert("class".into(), string("framebuffer"));
        props.insert("unix.subsystem".into(), string("graphics"));

        println!(
            "sif: Publishing {} as freestanding framebuffer",
            device.describe()
        );
        let manager: &'static EntityManager = leak(create_entity("freestanding-fb", &props).await?);
        hel::spawn(serve_freestanding(manager, device));
    }

    Ok(())
}
