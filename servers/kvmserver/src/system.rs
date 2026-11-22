use std::sync::Arc;

use async_trait::async_trait;
use bragi::Message;
use managarm::fs::{
    bindings as fs,
    server::{File, ServeError},
};

use crate::vm::VmFile;
use crate::{install_file, send_response, unsupported_ioctl};

const KVM_API_VERSION: u64 = 12;

pub fn check_extension(_cap: u64) -> u64 {
    // We do not report any capabilities yet.
    0
}

/// An open file of /dev/kvm, the system fd of the KVM API.
pub struct SystemFile {
    posix_lane: hel::Handle,
}

impl SystemFile {
    pub fn new(posix_lane: hel::Handle) -> Self {
        Self { posix_lane }
    }

    async fn create_vm(&self, machine_type: u64) -> Result<Arc<VmFile>, fs::Errors> {
        if machine_type != 0 {
            eprintln!("kvmserver: unsupported VM type {machine_type}");
            return Err(fs::Errors::IllegalArgument);
        }
        Ok(Arc::new(VmFile))
    }
}

#[async_trait(?Send)]
impl File for SystemFile {
    async fn ioctl(
        &self,
        id: u32,
        msg: Vec<u8>,
        conversation: hel::Handle,
    ) -> Result<(), ServeError> {
        match id {
            fs::KvmGetApiVersionRequest::MESSAGE_ID => {
                send_response(&conversation, fs::Errors::Success, KVM_API_VERSION).await
            }
            fs::KvmCheckExtensionRequest::MESSAGE_ID => {
                let req: fs::KvmCheckExtensionRequest =
                    bragi::head_from_bytes(&msg).map_err(ServeError::Malformed)?;
                let value = check_extension(req.extension());
                send_response(&conversation, fs::Errors::Success, value).await
            }
            fs::KvmCreateVmRequest::MESSAGE_ID => {
                let req: fs::KvmCreateVmRequest =
                    bragi::head_from_bytes(&msg).map_err(ServeError::Malformed)?;
                let credentials =
                    hel::submit_async(&conversation, hel::ExtractCredentials).await??;
                let vm = match self.create_vm(req.machine_type()).await {
                    Ok(vm) => vm,
                    Err(e) => return send_response(&conversation, e, 0).await,
                };
                match install_file(&self.posix_lane, credentials, vm).await {
                    Ok(fd) => send_response(&conversation, fs::Errors::Success, fd as u64).await,
                    Err(e) => {
                        eprintln!("kvmserver: failed to install VM fd: {e:#}");
                        send_response(&conversation, fs::Errors::InternalError, 0).await
                    }
                }
            }
            _ => unsupported_ioctl(&conversation, "system", id).await,
        }
    }
}
