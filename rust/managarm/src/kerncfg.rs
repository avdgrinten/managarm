use std::sync::OnceLock;

use anyhow::{Context, anyhow, bail};

use crate::mbus;
use crate::shm::{Consumer, Framing, Mode, Producer};

bragi::include_binding!(mod bindings = "kerncfg.rs");

/// Fetches the kernel command line by querying the `kerncfg` mbus entity.
pub async fn get_cmdline() -> anyhow::Result<String> {
    let lane = open_kerncfg_lane().await?;

    let head = bragi::head_to_bytes(&bindings::GetCmdlineRequest::new())?;
    let (offer, (_send_head, recv)) = hel::submit_async(
        &lane,
        hel::Offer::new_with_lane((hel::SendBuffer::new(&head), hel::ReceiveInline)),
    )
    .await?;

    let recv_data = recv?;
    let conversation_lane = offer?.context("kerncfg did not offer a lane")?;

    let response: bindings::SvrResponse = bragi::head_from_bytes(&recv_data)?;
    if response.error() != bindings::Error::Success {
        bail!("kerncfg returned error {:?}", response.error());
    }

    let size = response.size().context("kerncfg response missing size")? as usize;
    let mut buffer = vec![0u8; size];
    hel::submit_async(&conversation_lane, hel::ReceiveBuffer::new(&mut buffer)).await??;

    Ok(String::from_utf8(buffer).context("kernel cmdline is not valid UTF-8")?)
}

static KERNCFG_LANE: OnceLock<hel::Handle> = OnceLock::new();

// thor serves any number of requests on a kerncfg lane, hence it is opened only once.
async fn open_kerncfg_lane() -> anyhow::Result<&'static hel::Handle> {
    if let Some(lane) = KERNCFG_LANE.get() {
        return Ok(lane);
    }

    let mut enumerator = mbus::Enumerator::new(mbus::Filter::Equals("class", "kerncfg"));
    let (_overflow, events) = enumerator
        .next_events()
        .await
        .map_err(|e| anyhow!("mbus enumeration failed: {e}"))?;
    let event = events
        .into_iter()
        .next()
        .context("no kerncfg entity found")?;
    let lane = event
        .entity()
        .get_remote_lane()
        .await
        .map_err(|e| anyhow!("failed to open kerncfg lane: {e}"))?;
    // A concurrent caller may have cached its lane first; either lane works.
    Ok(KERNCFG_LANE.get_or_init(|| lane))
}

/// The amount of memory in the system, in bytes.
pub struct MemoryInformation {
    pub total_memory: usize,
    pub available_memory: usize,
}

pub async fn get_memory_information() -> anyhow::Result<MemoryInformation> {
    let lane = open_kerncfg_lane().await?;

    let head = bragi::head_to_bytes(&bindings::GetMemoryInformationRequest::new())?;
    let (_offer, (_send_head, recv)) = hel::submit_async(
        &lane,
        hel::Offer::new_with_lane((hel::SendBuffer::new(&head), hel::ReceiveInline)),
    )
    .await?;

    let recv_data = recv?;
    let response: bindings::GetMemoryInformationResponse = bragi::head_from_bytes(&recv_data)?;
    if response.error() != bindings::Error::Success {
        bail!("kerncfg returned error {:?}", response.error());
    }

    Ok(MemoryInformation {
        total_memory: (response.total_usable_memory() * response.memory_unit()) as usize,
        available_memory: (response.available_memory() * response.memory_unit()) as usize,
    })
}

pub async fn get_acpi_rsdp() -> anyhow::Result<u64> {
    let lane = open_kerncfg_lane().await?;

    let head = bragi::head_to_bytes(&bindings::GetAcpiRsdpRequest::new())?;
    let (_offer, (_send_head, recv)) = hel::submit_async(
        &lane,
        hel::Offer::new_with_lane((hel::SendBuffer::new(&head), hel::ReceiveInline)),
    )
    .await?;

    let recv_data = recv?;
    let response: bindings::GetAcpiRsdpResponse = bragi::head_from_bytes(&recv_data)?;
    if response.error() != bindings::Error::Success {
        bail!("kerncfg returned error {:?}", response.error());
    }

    Ok(response.rsdp())
}

/// Returns the physical address and size of the device tree blob, or zeroes if there is none.
pub async fn get_device_tree() -> anyhow::Result<(u64, u64)> {
    let lane = open_kerncfg_lane().await?;

    let head = bragi::head_to_bytes(&bindings::GetDeviceTreeRequest::new())?;
    let (_offer, (_send_head, recv)) = hel::submit_async(
        &lane,
        hel::Offer::new_with_lane((hel::SendBuffer::new(&head), hel::ReceiveInline)),
    )
    .await?;

    let recv_data = recv?;
    let response: bindings::GetDeviceTreeResponse = bragi::head_from_bytes(&recv_data)?;
    if response.error() != bindings::Error::Success {
        bail!("kerncfg returned error {:?}", response.error());
    }

    Ok((response.address(), response.size()))
}

/// A kernel I/O channel that this server provides: the kernel writes its output to `output`
/// (and flushes the ring when output needs to be written out) and reads its input from `input`.
pub struct IoChannel {
    pub output: Consumer,
    pub input: Option<Producer>,
}

/// Registers a kernel I/O channel with the given tag; the kernel chooses the ring sizes.
/// Without `input`, the channel is output-only.
pub async fn provide_io_channel(
    tag: &str,
    descriptive_tag: &str,
    input: bool,
) -> anyhow::Result<IoChannel> {
    let lane = open_kerncfg_lane().await?;

    let request = bindings::ProvideIoChannelRequest::new(
        tag.to_string(),
        descriptive_tag.to_string(),
        input as u8,
    );
    let head = bragi::head_to_bytes(&request)?;
    let (offer, (_send_head, recv)) = hel::submit_async(
        &lane,
        hel::Offer::new_with_lane((hel::SendBuffer::new(&head), hel::ReceiveInline)),
    )
    .await?;

    let recv_data = recv?;
    let conversation = offer?.context("kerncfg did not offer a lane")?;
    let response: bindings::ProvideIoChannelResponse = bragi::head_from_bytes(&recv_data)?;
    if response.error() != bindings::Error::Success {
        bail!("kerncfg rejected I/O channel {tag}: {:?}", response.error());
    }

    let output = Consumer::establish(&conversation, Mode::Reliable, Framing::Stream).await?;
    let input = if input {
        Some(Producer::establish(&conversation, Mode::Reliable, Framing::Stream).await?)
    } else {
        None
    };

    Ok(IoChannel { output, input })
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum KernelDeviceResourceKind {
    Mmio,
    Io,
}

/// A physical address range or I/O port range that a kernel driver accesses.
#[derive(Debug, Clone, Copy)]
pub struct KernelDeviceResource {
    pub kind: KernelDeviceResourceKind,
    pub base: u64,
    pub length: u64,
}

/// Geometry of a framebuffer that the kernel draws its boot screen to.
#[derive(Debug, Clone, Copy)]
pub struct KernelFramebuffer {
    pub pitch: u32,
    pub width: u32,
    pub height: u32,
    pub bpp: u32,
    pub fb_type: u32,
}

/// A device that the kernel drives itself, see thor's KernelDevice registry.
#[derive(Debug, Clone)]
pub struct KernelDevice {
    pub id: u64,
    pub name: String,
    pub resources: Vec<KernelDeviceResource>,
    pub framebuffer: Option<KernelFramebuffer>,
}

/// Lists the devices that the kernel drives itself, with the resources they occupy.
pub async fn list_kernel_devices() -> anyhow::Result<Vec<KernelDevice>> {
    let lane = open_kerncfg_lane().await?;

    let head = bragi::head_to_bytes(&bindings::ListKernelDevicesRequest::new())?;
    let (offer, (_send_head, recv)) = hel::submit_async(
        &lane,
        hel::Offer::new_with_lane((hel::SendBuffer::new(&head), hel::ReceiveInline)),
    )
    .await?;

    let recv_data = recv?;
    let conversation_lane = offer?.context("kerncfg did not offer a lane")?;
    let preamble = bragi::preamble_from_bytes(&recv_data)?;
    let mut tail_buffer = vec![0; preamble.tail_size() as usize];
    hel::submit_async(
        &conversation_lane,
        hel::ReceiveBuffer::new(&mut tail_buffer),
    )
    .await??;

    let response: bindings::ListKernelDevicesResponse =
        bragi::head_tail_from_bytes(&recv_data, &tail_buffer)?;
    if response.error() != bindings::Error::Success {
        bail!("kerncfg returned error {:?}", response.error());
    }

    let mut devices: Vec<KernelDevice> = response
        .devices()
        .unwrap_or_default()
        .iter()
        .map(|device| KernelDevice {
            id: device.id(),
            name: device.name().to_string(),
            resources: Vec::new(),
            framebuffer: None,
        })
        .collect();
    for resource in response.resources().unwrap_or_default() {
        let device = devices
            .iter_mut()
            .find(|device| device.id == resource.device())
            .context("kernel device resource refers to an unknown device")?;
        device.resources.push(KernelDeviceResource {
            kind: match resource.kind() {
                bindings::KernelDeviceResourceKind::Mmio => KernelDeviceResourceKind::Mmio,
                bindings::KernelDeviceResourceKind::Io => KernelDeviceResourceKind::Io,
            },
            base: resource.base(),
            length: resource.length(),
        });
    }
    for fb in response.framebuffers().unwrap_or_default() {
        let device = devices
            .iter_mut()
            .find(|device| device.id == fb.device())
            .context("kernel framebuffer refers to an unknown device")?;
        device.framebuffer = Some(KernelFramebuffer {
            pitch: fb.pitch(),
            width: fb.width(),
            height: fb.height(),
            bpp: fb.bpp(),
            fb_type: fb.fb_type(),
        });
    }

    Ok(devices)
}

async fn kernel_device_request<M: bragi::Message>(request: &M) -> anyhow::Result<()> {
    let lane = open_kerncfg_lane().await?;

    let head = bragi::head_to_bytes(request)?;
    let (_offer, (_send_head, recv)) = hel::submit_async(
        &lane,
        hel::Offer::new_with_lane((hel::SendBuffer::new(&head), hel::ReceiveInline)),
    )
    .await?;

    let recv_data = recv?;
    let response: bindings::SvrResponse = bragi::head_from_bytes(&recv_data)?;
    if response.error() != bindings::Error::Success {
        bail!("kerncfg returned error {:?}", response.error());
    }

    Ok(())
}

/// Keeps the kernel driver off the device until [`resume_kernel_device`]; returns once no
/// access of the driver is in flight anymore. Suspensions nest.
pub async fn suspend_kernel_device(id: u64) -> anyhow::Result<()> {
    kernel_device_request(&bindings::SuspendKernelDeviceRequest::new(id)).await
}

pub async fn resume_kernel_device(id: u64) -> anyhow::Result<()> {
    kernel_device_request(&bindings::ResumeKernelDeviceRequest::new(id)).await
}

/// Makes the kernel give up the device for good, e.g., before a driver takes it over.
pub async fn claim_kernel_device(id: u64) -> anyhow::Result<()> {
    kernel_device_request(&bindings::ClaimKernelDeviceRequest::new(id)).await
}
