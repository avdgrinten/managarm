use async_trait::async_trait;
use bragi::Message;
use managarm::fs::{
    bindings as fs,
    server::{File, ServeError},
};

use crate::system::check_extension;
use crate::{send_response, unsupported_ioctl};

/// A VM created by KVM_CREATE_VM.
pub struct VmFile;

#[async_trait(?Send)]
impl File for VmFile {
    async fn ioctl(
        &self,
        id: u32,
        msg: Vec<u8>,
        conversation: hel::Handle,
    ) -> Result<(), ServeError> {
        match id {
            fs::KvmCheckExtensionRequest::MESSAGE_ID => {
                let req: fs::KvmCheckExtensionRequest =
                    bragi::head_from_bytes(&msg).map_err(ServeError::Malformed)?;
                let value = check_extension(req.extension());
                send_response(&conversation, fs::Errors::Success, value).await
            }
            _ => unsupported_ioctl(&conversation, "VM", id).await,
        }
    }
}
