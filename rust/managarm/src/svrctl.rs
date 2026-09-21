use anyhow::{Context, anyhow, bail};
use hel::Handle;
use std::mem::ManuallyDrop;
use std::sync::LazyLock;

use crate::mbus;
use crate::ringbuf::{self, Consumer, Framing, Mode, Producer, RingParameters, RingSpec, Role};

bragi::include_binding!(mod bindings = "svrctl.rs");

#[repr(i32)]
pub enum SvrctlSupercall {
    GetServerData = 64,
}

#[derive(Default)]
#[repr(C)]
struct ManagarmServerData {
    hardware_access: hel_sys::HelHandle,
    control_lane: hel_sys::HelHandle,
}

static SERVER_DATA: LazyLock<ManagarmServerData> = LazyLock::new(|| {
    let mut server_data = ManagarmServerData::default();
    let result = unsafe {
        hel_sys::helSyscall2(
            hel_sys::kHelCallSuper as i32 + SvrctlSupercall::GetServerData as i32,
            (&raw mut server_data).addr() as u64,
            std::mem::size_of::<ManagarmServerData>() as u64,
        )
    };

    if result != hel_sys::kHelErrNone as i32 {
        let error_string = unsafe { hel_sys::_helErrorString(result) };
        let error_cstr = unsafe { std::ffi::CStr::from_ptr(error_string) };

        panic!("Failed to get server data: {error_cstr:?}");
    }

    server_data
});

pub fn hardware_access_handle() -> &'static Handle {
    static HANDLE: LazyLock<ManuallyDrop<Handle>> = LazyLock::new(|| unsafe {
        ManuallyDrop::new(Handle::from_raw(SERVER_DATA.hardware_access))
    });

    &HANDLE
}

/// Finds the kernel's `svrctl` entity and returns its remote lane.
async fn open_svrctl_lane() -> anyhow::Result<Handle> {
    let mut enumerator = mbus::Enumerator::new(mbus::Filter::Equals("class", "svrctl"));
    let (_overflow, events) = enumerator
        .next_events()
        .await
        .map_err(|e| anyhow!("mbus enumeration failed: {e}"))?;
    let event = events
        .into_iter()
        .next()
        .context("no svrctl entity found")?;
    event
        .entity()
        .get_remote_lane()
        .await
        .map_err(|e| anyhow!("failed to open svrctl lane: {e}"))
}

/// A kernel I/O channel that this server provides: the kernel writes its output to `output`
/// (and flushes the ring when output needs to be written out) and reads its input from `input`.
pub struct IoChannel {
    pub output: Consumer,
    pub input: Option<Producer>,
}

/// Registers a kernel I/O channel with the given tag. Ring sizes must be page-aligned powers
/// of two; an `in_ring_size` of zero creates an output-only channel.
pub async fn provide_io_channel(
    tag: &str,
    descriptive_tag: &str,
    out_ring_size: u64,
    in_ring_size: u64,
) -> anyhow::Result<IoChannel> {
    let svrctl = open_svrctl_lane().await?;

    let request = bindings::ProvideIoChannelRequest::new(
        tag.to_string(),
        descriptive_tag.to_string(),
        out_ring_size,
        in_ring_size,
    );
    let head = bragi::head_to_bytes(&request)?;
    let (offer, (_send_head, recv)) = hel::submit_async(
        &svrctl,
        hel::Offer::new_with_lane((hel::SendBuffer::new(&head), hel::ReceiveInline)),
    )
    .await?;

    let recv_data = recv?;
    let conversation = offer?.context("svrctl did not offer a lane")?;
    let response: bindings::ProvideIoChannelResponse = bragi::head_from_bytes(&recv_data)?;
    if response.error() != bindings::Errors::Success {
        bail!(
            "svrctl rejected I/O channel {tag}: {:?}",
            response.error()
        );
    }

    let stream_spec = |size: u64, role| RingSpec {
        params: RingParameters {
            mode: Mode::Reliable,
            framing: Framing::Stream,
            size: size as usize,
        },
        role,
    };
    let mut specs = vec![stream_spec(out_ring_size, Role::Consumer)];
    if in_ring_size != 0 {
        specs.push(stream_spec(in_ring_size, Role::Producer));
    }
    let mut rings = ringbuf::receive(&conversation, &specs).await?.into_iter();
    let output = rings.next().context("missing output ring")?.into_consumer();
    let input = rings.next().map(ringbuf::Ring::into_producer);

    Ok(IoChannel { output, input })
}
