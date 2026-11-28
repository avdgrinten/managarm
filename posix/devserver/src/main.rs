//! posix-devserver: device manager for POSIX devices.
//!
//! posix-devserver is responsible for:
//! - Enumerating devices from mbus
//! - Instructing posix-subsystem to create/destory device nodes
//! - Serving sysfs
//! - Emitting uevents

mod device;
mod subsystem;
mod sysfs;

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

use anyhow::{Result, bail};
use bragi::Message;
use hel::Handle;
use managarm::mbus;

use device::{Device, Model};
use sysfs::SysfsNode;

bragi::include_binding!(pub(crate) mod proto = "devserver.rs");

pub(crate) const EXPECT_LOCK: &str = "a devserver mutex was poisoned";

/// Set by the `devserver.trace` kernel command line option.
pub(crate) static TRACE: AtomicBool = AtomicBool::new(false);

// Statically assert that our device model objects are Send + Sync.
const _: () = {
    fn check<T: Send + Sync>() {}
    let _ = check::<Model>;
    let _ = check::<Device>;
    let _ = check::<SysfsNode>;
};

pub async fn log_subsystem_errors(name: &'static str, task: impl Future<Output = Result<()>>) {
    if let Err(e) = task.await {
        eprintln!("devserver: {name} subsystem failed: {e:#}");
    }
}

/// Starts the per-subsystem mbus enumeration loops.
fn start_discovery(model: Arc<Model>) {
    macro_rules! spawn_subsystem {
        ($name:literal, $run:path) => {
            hel::spawn(log_subsystem_errors($name, $run(model.clone())));
        };
    }
    spawn_subsystem!("pci", subsystem::pci::run);
    spawn_subsystem!("acpi", subsystem::acpi::run);
    spawn_subsystem!("usb", subsystem::usb::run);
    spawn_subsystem!("block", subsystem::block::run);
    spawn_subsystem!("nvme", subsystem::nvme::run);
    spawn_subsystem!("drm", subsystem::drm::run);
    spawn_subsystem!("graphics", subsystem::graphics::run);
    spawn_subsystem!("input", subsystem::input::run);
    spawn_subsystem!("kvm", subsystem::kvm::run);
    spawn_subsystem!("net", subsystem::net::run);
    spawn_subsystem!("usbmisc", subsystem::usbmisc::run);
    spawn_subsystem!("sound", subsystem::sound::run);
    spawn_subsystem!("power_supply", subsystem::power_supply::run);
    spawn_subsystem!("tty", subsystem::tty::run);
    spawn_subsystem!("generic", subsystem::generic::run);
    spawn_subsystem!("dmi", subsystem::dmi::run);
    #[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
    spawn_subsystem!("dt", subsystem::dt::run);
}

// --------------------------------------------------------------------------------------
// posix-subsystem <-> devserver protocol handling.
// --------------------------------------------------------------------------------------

async fn serve_client(lane: Handle) {
    loop {
        match handle_client_request(&lane).await {
            Ok(true) => {}
            Ok(false) => return,
            Err(e) => {
                eprintln!("devserver: error while serving a request: {e:?}");
                return;
            }
        }
    }
}

/// Handles a single request on a client lane. Returns false if the lane was shut down.
async fn handle_client_request(lane: &Handle) -> Result<bool> {
    let (conv, (head,)) = hel::submit_async(lane, hel::Accept::new((hel::ReceiveInline,))).await?;

    let conversation = match conv {
        Ok(Some(lane)) => lane,
        Ok(None) => bail!("accept did not yield a conversation lane"),
        Err(hel::Error::EndOfLane) | Err(hel::Error::LaneShutdown) => return Ok(false),
        Err(e) => return Err(e.into()),
    };
    let head = head?;

    let preamble = bragi::preamble_from_bytes(&head)?;
    match preamble.id() {
        proto::LaunchRequest::MESSAGE_ID => {
            handle_launch(conversation).await?;
        }
        id => {
            eprintln!("devserver: dismissing request with unexpected message ID {id}");
            hel::submit_async(&conversation, hel::Dismiss).await??;
        }
    }
    Ok(true)
}

async fn handle_launch(conversation: Handle) -> Result<()> {
    let (sysfs_local, sysfs_remote) = hel::create_stream()?;
    let (events_local, events_remote) = hel::create_stream()?;

    let model = Model::new(events_local);
    start_discovery(model.clone());

    hel::spawn(sysfs::serve_superblock(sysfs_local, model.root.clone()));

    let resp = proto::GenericResponse::new(proto::Errors::Success);
    let resp_head = bragi::head_to_bytes(&resp)?;
    let (send_resp, push_sysfs, push_events) = hel::submit_async(
        &conversation,
        (
            hel::SendBuffer::new(&resp_head),
            hel::PushDescriptor::new(
                &sysfs_remote,
                hel_sys::kHelRightInvoke | hel_sys::kHelRightManage,
            ),
            hel::PushDescriptor::new(
                &events_remote,
                hel_sys::kHelRightInvoke | hel_sys::kHelRightManage,
            ),
        ),
    )
    .await?;
    send_resp?;
    push_sysfs?;
    push_events?;
    Ok(())
}

async fn run() -> Result<()> {
    let cmdline = managarm::kerncfg::get_cmdline().await?;
    TRACE.store(
        cmdline
            .split_ascii_whitespace()
            .any(|opt| opt == "devserver.trace"),
        Ordering::Relaxed,
    );

    let mut properties = mbus::Properties::new();
    properties.insert(
        "class".to_string(),
        mbus::Item::String("devserver".to_string()),
    );
    let entity = mbus::create_entity("devserver", &properties).await?;

    loop {
        let (local, remote) = hel::create_stream()?;
        entity.serve_remote_lane(remote).await?;

        hel::spawn(serve_client(local));
    }
}

fn main() -> Result<()> {
    println!("devserver: Starting up");
    hel::block_on(run())?
}
