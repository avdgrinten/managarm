//! kvmserver: implements the Linux KVM API (/dev/kvm) on top of hel's virtualization API.
//!
//! The server publishes an mbus entity that the devserver's kvm subsystem turns into /dev/kvm.

mod system;
mod vm;

use std::sync::Arc;

use anyhow::{Context, Result};
use managarm::fs::{
    bindings as fs,
    server::{File, ServeError, serve_passthrough},
};
use managarm::mbus;

use system::SystemFile;

async fn send_response(
    conversation: &hel::Handle,
    error: fs::Errors,
    value: u64,
) -> Result<(), ServeError> {
    let head = bragi::head_to_bytes(&fs::KvmResponse::new(error, value))?;
    hel::submit_async(conversation, hel::SendBuffer::new(&head)).await??;
    Ok(())
}

async fn unsupported_ioctl(
    conversation: &hel::Handle,
    what: &str,
    id: u32,
) -> Result<(), ServeError> {
    eprintln!("kvmserver: unsupported {what} ioctl (message ID {id})");
    hel::submit_async(conversation, hel::Dismiss).await??;
    Ok(())
}

/// Serves `file` on a new lane and installs it as a file descriptor into the
/// process with the given credentials.
async fn install_file(
    posix_lane: &hel::Handle,
    credentials: [u8; 16],
    file: Arc<dyn File>,
) -> Result<i32> {
    let (local, remote) = hel::create_stream()?;
    hel::spawn(serve_passthrough(local, file));
    Ok(managarm::posix::serve_fd(posix_lane, credentials, &remote).await?)
}

/// Handles a request on the device lane.
async fn handle_device_request(lane: &hel::Handle) -> Result<bool> {
    let (accept, (head,)) =
        hel::submit_async(lane, hel::Accept::new((hel::ReceiveInline,))).await?;
    let conversation = match accept {
        Ok(Some(conversation)) => conversation,
        Ok(None) => anyhow::bail!("accept did not yield a conversation lane"),
        Err(hel::Error::EndOfLane | hel::Error::LaneShutdown) => return Ok(false),
        Err(e) => return Err(e.into()),
    };
    let req: fs::CntRequest = bragi::head_from_bytes(&head?)?;

    match req.req_type() {
        fs::CntReqType::DevOpen => {
            let (local, remote) = hel::create_stream()?;
            let mut resp = fs::SvrResponse::new(fs::Errors::Success);
            resp.set_caps(fs::FileCaps::FC_POSIX_LANE.value());
            let resp_head = bragi::head_to_bytes(&resp)?;
            let (send_resp, push_lane, pull_posix_lane) = hel::submit_async(
                &conversation,
                (
                    hel::SendBuffer::new(&resp_head),
                    hel::PushDescriptor::new(
                        &remote,
                        hel_sys::kHelRightInvoke | hel_sys::kHelRightManage,
                    ),
                    hel::PullDescriptor::new(hel_sys::kHelRightInvoke | hel_sys::kHelRightManage),
                ),
            )
            .await?;
            send_resp.and(push_lane)?;
            let posix_lane = pull_posix_lane?.context("POSIX did not push a lane")?;
            let file = Arc::new(SystemFile::new(posix_lane));
            hel::spawn(serve_passthrough(local, file));
        }
        req_type => {
            eprintln!("kvmserver: unexpected device request {req_type:?}");
            hel::submit_async(&conversation, hel::Dismiss).await??;
        }
    }

    Ok(true)
}

async fn serve_device(lane: hel::Handle) {
    loop {
        match handle_device_request(&lane).await {
            Ok(true) => {}
            Ok(false) => return,
            Err(e) => eprintln!("kvmserver: error on device lane: {e:#}"),
        }
    }
}

async fn run() -> Result<()> {
    let mut properties = mbus::Properties::new();
    properties.insert(
        "unix.subsystem".to_string(),
        mbus::Item::String("kvm".to_string()),
    );
    let entity = mbus::create_entity("kvm", &properties).await?;

    loop {
        let (local, remote) = hel::create_stream()?;
        entity.serve_remote_lane(remote).await?;
        hel::spawn(serve_device(local));
    }
}

fn main() -> Result<()> {
    // TODO: The KVM API differs between architectures, we only implement the x86 one.
    if !cfg!(target_arch = "x86_64") {
        println!("kvmserver: This architecture is not supported yet");
        return Ok(());
    }
    if hel::create_virtualized_space().is_err() {
        println!("kvmserver: No hardware virtualization support, exiting");
        return Ok(());
    }
    println!("kvmserver: Starting up");
    hel::block_on(run())?
}
