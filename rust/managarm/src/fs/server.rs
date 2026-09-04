//! Server-side implementation of the fs node and passthrough protocols.

use std::pin::Pin;
use std::sync::Arc;

use async_trait::async_trait;
use bragi::Message;

use super::bindings;

pub use bindings::Errors as Error;
pub use bindings::FileType;

/// Credentials of the process that issued a request.
pub type Credentials = [u8; 16];

/// Failures encountered while serving a request, as opposed to the protocol-level
/// [`Error`] that a file operation reports back to the client.
#[derive(Debug, thiserror::Error)]
pub enum ServeError {
    /// The lane was shut down; serving should stop.
    #[error("lane was shut down")]
    Shutdown,
    /// A received message could not be decoded (a protocol violation).
    #[error("malformed message: {0}")]
    Malformed(std::io::Error),
    /// A response could not be encoded.
    #[error("failed to encode a response: {0}")]
    Encode(#[from] std::io::Error),
    /// A Hel transport operation failed.
    #[error("transport operation failed: {0}")]
    Transport(#[from] hel::Error),
}

/// Stats of a node, as reported by [`Node::get_stats`].
#[derive(Debug, Clone, Copy, Default)]
pub struct NodeStats {
    pub file_size: u64,
    pub num_links: u64,
    pub mode: i32,
    pub uid: i64,
    pub gid: i64,
    pub atime_secs: i64,
    pub atime_nanos: i64,
    pub mtime_secs: i64,
    pub mtime_nanos: i64,
    pub ctime_secs: i64,
    pub ctime_nanos: i64,
}

/// A link to a child node, as resolved by [`Node::get_link`] or created by
/// [`Node::mkdir`] and friends.
pub struct Child {
    pub node: Arc<dyn Node>,
    pub id: i64,
    pub file_type: FileType,
}

/// Result of a successful [`Node::traverse_links`].
pub struct TraverseResult {
    /// (node, id) of every path component that was resolved, in path order.
    pub nodes: Vec<(Arc<dyn Node>, i64)>,
    /// File type of the last resolved component.
    pub file_type: FileType,
    /// Number of path components that were consumed.
    pub links_traversed: u64,
}

/// A single directory entry, as returned by [`File::read_entries`].
pub struct DirEntry {
    pub name: String,
    pub inode: u64,
    pub offset: i64,
    pub file_type: FileType,
}

/// Operations that a file served via [`serve_passthrough`] can implement.
/// Unimplemented operations fail with [`Error::IllegalOperationTarget`].
#[allow(unused_variables)]
#[async_trait(?Send)]
pub trait File {
    async fn seek_abs(&self, offset: i64) -> Result<i64, Error> {
        Err(Error::IllegalOperationTarget)
    }

    async fn seek_rel(&self, offset: i64) -> Result<i64, Error> {
        Err(Error::IllegalOperationTarget)
    }

    async fn seek_eof(&self, offset: i64) -> Result<i64, Error> {
        Err(Error::IllegalOperationTarget)
    }

    /// Reads at most `buffer.len()` bytes into `buffer` and returns the
    /// number of bytes read.
    async fn read(&self, credentials: Credentials, buffer: &mut [u8]) -> Result<usize, Error> {
        Err(Error::IllegalOperationTarget)
    }

    /// Reads at most `buffer.len()` bytes at the given offset, without
    /// affecting the file offset.
    async fn pread(
        &self,
        credentials: Credentials,
        offset: i64,
        buffer: &mut [u8],
    ) -> Result<usize, Error> {
        Err(Error::IllegalOperationTarget)
    }

    /// Writes the bytes in `buffer` and returns the number of bytes written.
    async fn write(&self, credentials: Credentials, buffer: &[u8]) -> Result<usize, Error> {
        Err(Error::IllegalOperationTarget)
    }

    /// Writes the bytes in `buffer` at the given offset, without affecting
    /// the file offset.
    async fn pwrite(
        &self,
        credentials: Credentials,
        offset: i64,
        buffer: &[u8],
    ) -> Result<usize, Error> {
        Err(Error::IllegalOperationTarget)
    }

    async fn truncate(&self, size: u64) -> Result<(), Error> {
        Err(Error::IllegalOperationTarget)
    }

    /// Returns the next directory entry, or `None` at the end of the directory.
    async fn read_entries(&self) -> Result<Option<DirEntry>, Error> {
        Err(Error::IllegalOperationTarget)
    }
}

/// Operations that a filesystem node served via [`serve_node`] can implement.
/// Unimplemented operations fail with [`Error::IllegalOperationTarget`].
#[allow(unused_variables)]
#[async_trait(?Send)]
pub trait Node {
    async fn get_stats(&self) -> Result<NodeStats, Error> {
        Err(Error::IllegalOperationTarget)
    }

    /// Resolves a single link in this directory; `None` if the name does not exist.
    async fn get_link(&self, name: &str) -> Result<Option<Child>, Error> {
        Err(Error::IllegalOperationTarget)
    }

    /// Resolves as many of the given path components as possible, stopping at
    /// symlinks, obstructed links and the boundaries of this filesystem.
    async fn traverse_links(&self, path: &[String]) -> Result<TraverseResult, Error> {
        Err(Error::IllegalOperationTarget)
    }

    async fn open(&self, read: bool, write: bool, append: bool) -> Result<Arc<dyn File>, Error> {
        Err(Error::IllegalOperationTarget)
    }

    async fn read_symlink(&self) -> Result<String, Error> {
        Err(Error::IllegalOperationTarget)
    }

    async fn chmod(&self, mode: i32) -> Result<(), Error> {
        Err(Error::IllegalOperationTarget)
    }

    async fn mkdir(&self, name: &str, uid: i32, gid: i32, mode: i32) -> Result<Child, Error> {
        Err(Error::IllegalOperationTarget)
    }

    async fn symlink(&self, name: &str, target: &str) -> Result<Child, Error> {
        Err(Error::IllegalOperationTarget)
    }

    async fn unlink(&self, name: &str) -> Result<(), Error> {
        Err(Error::IllegalOperationTarget)
    }

    async fn rmdir(&self, name: &str) -> Result<(), Error> {
        Err(Error::IllegalOperationTarget)
    }

    /// Called when the client mounts another filesystem over the named link;
    /// [`Node::traverse_links`] must stop at obstructed links.
    async fn obstruct_link(&self, name: &str) -> Result<(), Error> {
        Err(Error::IllegalOperationTarget)
    }
}

/// Flattens the doubly-nested result of a single-action `submit_async`.
fn flatten<T>(result: hel::Result<hel::Result<T>>) -> hel::Result<T> {
    result.and_then(|inner| inner)
}

async fn send_response<M: Message>(
    conversation: &hel::Handle,
    resp: &M,
) -> Result<(), ServeError> {
    let head = bragi::head_to_bytes(resp)?;
    flatten(hel::submit_async(conversation, hel::SendBuffer::new(&head)).await)?;
    Ok(())
}

async fn send_response_with_tail<M: Message>(
    conversation: &hel::Handle,
    resp: &M,
) -> Result<(), ServeError> {
    let (head, tail) = bragi::head_tail_to_bytes(resp)?;
    let (send_head, send_tail) = hel::submit_async(
        conversation,
        (hel::SendBuffer::new(&head), hel::SendBuffer::new(&tail)),
    )
    .await?;
    send_head.and(send_tail)?;
    Ok(())
}

/// Receives the tail of a head/tail message on the conversation lane.
async fn receive_tail(
    conversation: &hel::Handle,
    tail_size: usize,
) -> Result<Vec<u8>, ServeError> {
    let mut tail = vec![0u8; tail_size];
    flatten(hel::submit_async(conversation, hel::ReceiveBuffer::new(&mut tail)).await)?;
    Ok(tail)
}

async fn extract_credentials(conversation: &hel::Handle) -> Result<Credentials, ServeError> {
    Ok(flatten(
        hel::submit_async(conversation, hel::ExtractCredentials).await,
    )?)
}

async fn handle_seek(
    conversation: hel::Handle,
    file: Arc<dyn File>,
    req: bindings::CntRequest,
) -> Result<(), ServeError> {
    let offset = req.rel_offset().unwrap_or(0);
    let result = match req.req_type() {
        bindings::CntReqType::SeekAbs => file.seek_abs(offset).await,
        bindings::CntReqType::SeekRel => file.seek_rel(offset).await,
        bindings::CntReqType::SeekEof => file.seek_eof(offset).await,
        _ => unreachable!(),
    };

    let resp = match result {
        Ok(offset) => {
            let mut resp = bindings::SvrResponse::new(Error::Success);
            resp.set_offset(offset as u64);
            resp
        }
        Err(e) => bindings::SvrResponse::new(e),
    };
    send_response(&conversation, &resp).await
}

async fn handle_cnt_request(
    conversation: hel::Handle,
    file: Arc<dyn File>,
    req: bindings::CntRequest,
) -> Result<(), ServeError> {
    match req.req_type() {
        bindings::CntReqType::SeekAbs
        | bindings::CntReqType::SeekRel
        | bindings::CntReqType::SeekEof => handle_seek(conversation, file, req).await,
        req_type => {
            eprintln!("managarm/fs: dismissing unexpected request type {req_type:?}");
            dismiss(conversation).await
        }
    }
}

async fn handle_read(
    conversation: hel::Handle,
    file: Arc<dyn File>,
    req: bindings::ReadRequest,
) -> Result<(), ServeError> {
    let credentials = extract_credentials(&conversation).await?;

    let mut buffer = vec![0u8; req.size() as usize];
    let (resp, size) = match file.read(credentials, &mut buffer).await {
        Ok(size) => {
            assert!(size <= buffer.len());
            (bindings::SvrResponse::new(Error::Success), size)
        }
        Err(e) => (bindings::SvrResponse::new(e), 0),
    };

    let head = bragi::head_to_bytes(&resp)?;
    let (send_head, send_data) = hel::submit_async(
        &conversation,
        (
            hel::SendBuffer::new(&head),
            hel::SendBuffer::new(&buffer[..size]),
        ),
    )
    .await?;
    send_head.and(send_data)?;
    Ok(())
}

async fn handle_pread(
    conversation: hel::Handle,
    file: Arc<dyn File>,
    req: bindings::PreadRequest,
) -> Result<(), ServeError> {
    let credentials = extract_credentials(&conversation).await?;

    let mut buffer = vec![0u8; req.size() as usize];

    // On success the client expects the data after the response; on error it
    // expects the response alone.
    match file.pread(credentials, req.offset(), &mut buffer).await {
        Ok(size) => {
            assert!(size <= buffer.len());
            let resp = bindings::SvrResponse::new(Error::Success);
            let head = bragi::head_to_bytes(&resp)?;
            let (send_head, send_data) = hel::submit_async(
                &conversation,
                (
                    hel::SendBuffer::new(&head),
                    hel::SendBuffer::new(&buffer[..size]),
                ),
            )
            .await?;
            send_head.and(send_data)?;
            Ok(())
        }
        Err(e) => send_response(&conversation, &bindings::SvrResponse::new(e)).await,
    }
}

async fn handle_write(
    conversation: hel::Handle,
    file: Arc<dyn File>,
    req: bindings::WriteRequest,
) -> Result<(), ServeError> {
    let mut buffer = vec![0u8; req.size() as usize];
    let (credentials, received) = hel::submit_async(
        &conversation,
        (
            hel::ExtractCredentials,
            hel::ReceiveBuffer::new(&mut buffer),
        ),
    )
    .await?;
    let credentials = credentials?;
    let received = received?;

    let resp = match file.write(credentials, &buffer[..received]).await {
        Ok(size) => {
            let mut resp = bindings::SvrResponse::new(Error::Success);
            resp.set_size(size as i64);
            resp
        }
        Err(e) => bindings::SvrResponse::new(e),
    };
    send_response(&conversation, &resp).await
}

async fn handle_pwrite(
    conversation: hel::Handle,
    file: Arc<dyn File>,
    req: bindings::PwriteRequest,
) -> Result<(), ServeError> {
    let mut buffer = vec![0u8; req.size() as usize];
    let (credentials, received) = hel::submit_async(
        &conversation,
        (
            hel::ExtractCredentials,
            hel::ReceiveBuffer::new(&mut buffer),
        ),
    )
    .await?;
    let credentials = credentials?;
    let received = received?;

    let resp = match file
        .pwrite(credentials, req.offset(), &buffer[..received])
        .await
    {
        Ok(size) => {
            let mut resp = bindings::SvrResponse::new(Error::Success);
            resp.set_size(size as i64);
            resp
        }
        Err(e) => bindings::SvrResponse::new(e),
    };
    send_response(&conversation, &resp).await
}

async fn handle_truncate(
    conversation: hel::Handle,
    file: Arc<dyn File>,
    req: bindings::TruncateRequest,
) -> Result<(), ServeError> {
    let resp = match file.truncate(req.size()).await {
        Ok(()) => bindings::SvrResponse::new(Error::Success),
        Err(e) => bindings::SvrResponse::new(e),
    };
    send_response(&conversation, &resp).await
}

async fn handle_read_entries(
    conversation: hel::Handle,
    file: Arc<dyn File>,
    _req: bindings::ReadEntriesRequest,
) -> Result<(), ServeError> {
    let resp = match file.read_entries().await {
        Ok(Some(entry)) => bindings::ReadEntriesResponse::new(
            Error::Success,
            entry.file_type,
            entry.inode,
            entry.offset,
            entry.name,
        ),
        Ok(None) => {
            bindings::ReadEntriesResponse::new(Error::EndOfFile, FileType::REGULAR, 0, 0, String::new())
        }
        Err(e) => bindings::ReadEntriesResponse::new(e, FileType::REGULAR, 0, 0, String::new()),
    };
    send_response_with_tail(&conversation, &resp).await
}

// TODO: Fire a cancellation event once cancellation support is implemented.
async fn handle_cancel(
    conversation: hel::Handle,
    req: bindings::CancelOperation,
) -> Result<(), ServeError> {
    extract_credentials(&conversation).await?;
    eprintln!(
        "managarm/fs: ignoring CancelOperation for cancellation ID {}",
        req.cancellation_id()
    );
    // CancelOperation expects no response.
    Ok(())
}

async fn dismiss(conversation: hel::Handle) -> Result<(), ServeError> {
    flatten(hel::submit_async(&conversation, hel::Dismiss).await)?;
    Ok(())
}

/// Logs any error produced by a request handler running on a detached task.
async fn log_errors(fut: impl Future<Output = Result<(), ServeError>>) {
    if let Err(e) = fut.await {
        eprintln!("managarm/fs: {e}");
    }
}

fn parse_and_spawn<M, F, Fut>(
    head: &[u8],
    conversation: hel::Handle,
    handler: F,
) -> Result<(), ServeError>
where
    M: Default + Message,
    F: FnOnce(hel::Handle, M) -> Fut,
    Fut: Future<Output = Result<(), ServeError>> + 'static,
{
    let req = bragi::head_from_bytes::<M>(head).map_err(ServeError::Malformed)?;
    hel::spawn(log_errors(handler(conversation, req)));
    Ok(())
}

/// Accepts a single conversation, receives the request head and dispatches it to
/// a detached handler task.
///
/// Returns [`ServeError::Shutdown`] once the lane has been shut down; every other
/// error is a per-request failure that leaves the serve loop able to continue.
async fn dispatch_request(lane: &hel::Handle, file: &Arc<dyn File>) -> Result<(), ServeError> {
    let (accept, (head,)) =
        hel::submit_async(lane, hel::Accept::new((hel::ReceiveInline,))).await?;

    // Only the accept action can signal shutdown; any other failure is an IPC error.
    let conversation = match accept {
        Ok(Some(conversation)) => conversation,
        Ok(None) => return Err(ServeError::Transport(hel::Error::IllegalState)),
        Err(hel::Error::LaneShutdown | hel::Error::EndOfLane) => return Err(ServeError::Shutdown),
        Err(e) => return Err(ServeError::Transport(e)),
    };
    let head = head?;

    let preamble = bragi::preamble_from_bytes(&head).map_err(ServeError::Malformed)?;

    match preamble.id() {
        bindings::CntRequest::MESSAGE_ID => {
            let file = file.clone();
            parse_and_spawn(&head, conversation, move |conversation, req| {
                handle_cnt_request(conversation, file, req)
            })?;
        }
        bindings::ReadRequest::MESSAGE_ID => {
            let file = file.clone();
            parse_and_spawn(&head, conversation, move |conversation, req| {
                handle_read(conversation, file, req)
            })?;
        }
        bindings::PreadRequest::MESSAGE_ID => {
            let file = file.clone();
            parse_and_spawn(&head, conversation, move |conversation, req| {
                handle_pread(conversation, file, req)
            })?;
        }
        bindings::WriteRequest::MESSAGE_ID => {
            let file = file.clone();
            parse_and_spawn(&head, conversation, move |conversation, req| {
                handle_write(conversation, file, req)
            })?;
        }
        bindings::PwriteRequest::MESSAGE_ID => {
            let file = file.clone();
            parse_and_spawn(&head, conversation, move |conversation, req| {
                handle_pwrite(conversation, file, req)
            })?;
        }
        bindings::TruncateRequest::MESSAGE_ID => {
            let file = file.clone();
            parse_and_spawn(&head, conversation, move |conversation, req| {
                handle_truncate(conversation, file, req)
            })?;
        }
        bindings::ReadEntriesRequest::MESSAGE_ID => {
            let file = file.clone();
            parse_and_spawn(&head, conversation, move |conversation, req| {
                handle_read_entries(conversation, file, req)
            })?;
        }
        bindings::CancelOperation::MESSAGE_ID => {
            parse_and_spawn(&head, conversation, handle_cancel)?;
        }
        // A decodable but unhandled message ID: unsupported rather than malformed.
        // Dismiss so that the client observes an error instead of hanging.
        id => {
            eprintln!("managarm/fs: dismissing request with unexpected message ID {id}");
            hel::spawn(log_errors(dismiss(conversation)));
        }
    }
    Ok(())
}

/// Serves the fs passthrough protocol on the given lane until it is shut down.
///
/// Each request is handled on a detached task so that a blocking operation
/// (e.g. a read that waits for data) does not stall subsequent requests.
pub async fn serve_passthrough(lane: hel::Handle, file: Arc<dyn File>) {
    loop {
        match dispatch_request(&lane, &file).await {
            Ok(()) => {}
            Err(ServeError::Shutdown) => return,
            Err(e) => eprintln!("managarm/fs: {e}"),
        }
    }
}

/// Serves the control lane of an open file. No operations are defined on this
/// lane; the client shutting it down signals that the file was closed.
async fn serve_control(lane: hel::Handle) {
    loop {
        let (accept, (_head,)) =
            match hel::submit_async(&lane, hel::Accept::new((hel::ReceiveInline,))).await {
                Ok(results) => results,
                Err(e) => {
                    eprintln!("managarm/fs: transport error on control lane: {e}");
                    return;
                }
            };
        match accept {
            Ok(Some(conversation)) => {
                if let Err(e) = dismiss(conversation).await {
                    eprintln!("managarm/fs: {e}");
                }
            }
            Ok(None) => return,
            Err(hel::Error::LaneShutdown | hel::Error::EndOfLane) => return,
            Err(e) => {
                eprintln!("managarm/fs: transport error on control lane: {e}");
                return;
            }
        }
    }
}

/// Serves the fs node protocol on the given lane on a detached task.
pub fn serve_node(lane: hel::Handle, node: Arc<dyn Node>) {
    // Box the future: serve_node() is invoked recursively from its own handlers.
    let fut: Pin<Box<dyn Future<Output = ()>>> = Box::pin(serve_node_loop(lane, node));
    hel::spawn(fut);
}

async fn serve_node_loop(lane: hel::Handle, node: Arc<dyn Node>) {
    loop {
        match dispatch_node_request(&lane, &node).await {
            Ok(()) => {}
            Err(ServeError::Shutdown) => return,
            Err(e) => eprintln!("managarm/fs: {e}"),
        }
    }
}

/// Creates a fresh lane pair whose local end serves `node` and returns the remote end.
fn serve_new_node_lane(node: Arc<dyn Node>) -> Result<hel::Handle, ServeError> {
    let (local, remote) = hel::create_stream()?;
    serve_node(local, node);
    Ok(remote)
}

async fn handle_node_cnt_request(
    conversation: hel::Handle,
    node: Arc<dyn Node>,
    req: bindings::CntRequest,
) -> Result<(), ServeError> {
    match req.req_type() {
        bindings::CntReqType::NodeGetStats => {
            let resp = match node.get_stats().await {
                Ok(stats) => {
                    let mut resp = bindings::SvrResponse::new(Error::Success);
                    resp.set_file_size(stats.file_size);
                    resp.set_num_links(stats.num_links);
                    resp.set_mode(stats.mode);
                    resp.set_uid(stats.uid);
                    resp.set_gid(stats.gid);
                    resp.set_atime_secs(stats.atime_secs);
                    resp.set_atime_nanos(stats.atime_nanos);
                    resp.set_mtime_secs(stats.mtime_secs);
                    resp.set_mtime_nanos(stats.mtime_nanos);
                    resp.set_ctime_secs(stats.ctime_secs);
                    resp.set_ctime_nanos(stats.ctime_nanos);
                    resp
                }
                Err(e) => bindings::SvrResponse::new(e),
            };
            send_response(&conversation, &resp).await
        }
        bindings::CntReqType::NodeReadSymlink => {
            let (resp, target) = match node.read_symlink().await {
                Ok(target) => (bindings::SvrResponse::new(Error::Success), target),
                Err(e) => (bindings::SvrResponse::new(e), String::new()),
            };
            let head = bragi::head_to_bytes(&resp)?;
            let (send_resp, send_target) = hel::submit_async(
                &conversation,
                (
                    hel::SendBuffer::new(&head),
                    hel::SendBuffer::new(target.as_bytes()),
                ),
            )
            .await?;
            send_resp.and(send_target)?;
            Ok(())
        }
        bindings::CntReqType::NodeChmod => {
            let resp = match node.chmod(req.mode().unwrap_or(0)).await {
                Ok(()) => bindings::SvrResponse::new(Error::Success),
                Err(e) => bindings::SvrResponse::new(e),
            };
            send_response(&conversation, &resp).await
        }
        bindings::CntReqType::NodeSymlink => {
            let mut name = vec![0u8; req.name_length().unwrap_or(0) as usize];
            let mut target = vec![0u8; req.target_length().unwrap_or(0) as usize];
            let (recv_name, recv_target) = hel::submit_async(
                &conversation,
                (
                    hel::ReceiveBuffer::new(&mut name),
                    hel::ReceiveBuffer::new(&mut target),
                ),
            )
            .await?;
            recv_name.and(recv_target)?;

            let name = String::from_utf8_lossy(&name).into_owned();
            let target = String::from_utf8_lossy(&target).into_owned();
            match node.symlink(&name, &target).await {
                Ok(child) => {
                    let remote = serve_new_node_lane(child.node)?;
                    let mut resp = bindings::SvrResponse::new(Error::Success);
                    resp.set_id(child.id);
                    let head = bragi::head_to_bytes(&resp)?;
                    let (send_resp, push_node) = hel::submit_async(
                        &conversation,
                        (
                            hel::SendBuffer::new(&head),
                            hel::PushDescriptor::new(&remote, hel_sys::kHelRightInvoke | hel_sys::kHelRightManage),
                        ),
                    )
                    .await?;
                    send_resp.and(push_node)?;
                    Ok(())
                }
                Err(e) => send_response(&conversation, &bindings::SvrResponse::new(e)).await,
            }
        }
        req_type => {
            eprintln!("managarm/fs: dismissing unexpected node request type {req_type:?}");
            dismiss(conversation).await
        }
    }
}

async fn handle_get_link(
    conversation: hel::Handle,
    node: Arc<dyn Node>,
    head: Vec<u8>,
    tail_size: usize,
) -> Result<(), ServeError> {
    let tail = receive_tail(&conversation, tail_size).await?;
    let req: bindings::GetLinkRequest =
        bragi::head_tail_from_bytes(&head, &tail).map_err(ServeError::Malformed)?;

    match node.get_link(req.path()).await {
        Ok(Some(child)) => {
            let remote = serve_new_node_lane(child.node)?;
            let mut resp = bindings::SvrResponse::new(Error::Success);
            resp.set_id(child.id);
            resp.set_file_type(child.file_type);
            let head = bragi::head_to_bytes(&resp)?;
            let (send_resp, push_node) = hel::submit_async(
                &conversation,
                (
                    hel::SendBuffer::new(&head),
                    hel::PushDescriptor::new(&remote, hel_sys::kHelRightInvoke | hel_sys::kHelRightManage),
                ),
            )
            .await?;
            send_resp.and(push_node)?;
            Ok(())
        }
        Ok(None) => send_response(&conversation, &bindings::SvrResponse::new(Error::FileNotFound)).await,
        Err(e) => send_response(&conversation, &bindings::SvrResponse::new(e)).await,
    }
}

async fn handle_traverse_links(
    conversation: hel::Handle,
    node: Arc<dyn Node>,
    head: Vec<u8>,
    tail_size: usize,
) -> Result<(), ServeError> {
    let tail = receive_tail(&conversation, tail_size).await?;
    let req: bindings::NodeTraverseLinksRequest =
        bragi::head_tail_from_bytes(&head, &tail).map_err(ServeError::Malformed)?;

    let result = match node.traverse_links(req.path_segments()).await {
        Ok(result) => result,
        Err(e) => {
            let resp = bindings::NodeTraverseLinksResponse::new(
                e,
                0,
                FileType::REGULAR,
                0,
                Vec::new(),
                Vec::new(),
            );
            return send_response_with_tail(&conversation, &resp).await;
        }
    };

    let ids: Vec<i64> = result.nodes.iter().map(|&(_, id)| id).collect();
    // We do not track directory mutation serials. This server never advertises
    // MC_CLIENT_EXCLUSIVE_NAMESPACE, hence no client caches these lookups.
    let serials = vec![0; ids.len()];
    let resp = bindings::NodeTraverseLinksResponse::new(
        Error::Success,
        result.links_traversed,
        result.file_type,
        0,
        ids,
        serials,
    );

    // The node lanes are pushed onto a dedicated lane so that the client can
    // pull them outside of this conversation.
    let (push_local, push_remote) = hel::create_stream()?;
    let (resp_head, resp_tail) = bragi::head_tail_to_bytes(&resp)?;
    let (send_head, send_tail, push_lane) = hel::submit_async(
        &conversation,
        (
            hel::SendBuffer::new(&resp_head),
            hel::SendBuffer::new(&resp_tail),
            hel::PushDescriptor::new(&push_remote, hel_sys::kHelRightInvoke | hel_sys::kHelRightManage),
        ),
    )
    .await?;
    send_head.and(send_tail).and(push_lane)?;

    for (child, _) in result.nodes {
        let remote = serve_new_node_lane(child)?;
        flatten(hel::submit_async(&push_local, hel::PushDescriptor::new(&remote, hel_sys::kHelRightInvoke | hel_sys::kHelRightManage)).await)?;
    }
    Ok(())
}

async fn handle_node_open(
    conversation: hel::Handle,
    node: Arc<dyn Node>,
    req: bindings::NodeOpenRequest,
) -> Result<(), ServeError> {
    match node.open(req.read() != 0, req.write() != 0, req.append() != 0).await {
        Ok(file) => {
            let (control_local, control_remote) = hel::create_stream()?;
            let (pt_local, pt_remote) = hel::create_stream()?;
            hel::spawn(serve_control(control_local));
            hel::spawn(serve_passthrough(pt_local, file));

            let resp = bindings::SvrResponse::new(Error::Success);
            let head = bragi::head_to_bytes(&resp)?;
            let (send_resp, push_control, push_pt) = hel::submit_async(
                &conversation,
                (
                    hel::SendBuffer::new(&head),
                    hel::PushDescriptor::new(&control_remote, hel_sys::kHelRightInvoke | hel_sys::kHelRightManage),
                    hel::PushDescriptor::new(&pt_remote, hel_sys::kHelRightInvoke | hel_sys::kHelRightManage),
                ),
            )
            .await?;
            send_resp.and(push_control).and(push_pt)?;
            Ok(())
        }
        Err(e) => send_response(&conversation, &bindings::SvrResponse::new(e)).await,
    }
}

async fn handle_mkdir(
    conversation: hel::Handle,
    node: Arc<dyn Node>,
    head: Vec<u8>,
    tail_size: usize,
) -> Result<(), ServeError> {
    let tail = receive_tail(&conversation, tail_size).await?;
    let req: bindings::MkdirRequest =
        bragi::head_tail_from_bytes(&head, &tail).map_err(ServeError::Malformed)?;

    match node.mkdir(req.path(), req.uid(), req.gid(), req.mode()).await {
        Ok(child) => {
            let remote = serve_new_node_lane(child.node)?;
            let mut resp = bindings::SvrResponse::new(Error::Success);
            resp.set_id(child.id);
            let head = bragi::head_to_bytes(&resp)?;
            let (send_resp, push_node) = hel::submit_async(
                &conversation,
                (
                    hel::SendBuffer::new(&head),
                    hel::PushDescriptor::new(&remote, hel_sys::kHelRightInvoke | hel_sys::kHelRightManage),
                ),
            )
            .await?;
            send_resp.and(push_node)?;
            Ok(())
        }
        Err(e) => send_response(&conversation, &bindings::SvrResponse::new(e)).await,
    }
}

async fn handle_unlink(
    conversation: hel::Handle,
    node: Arc<dyn Node>,
    head: Vec<u8>,
    tail_size: usize,
) -> Result<(), ServeError> {
    let tail = receive_tail(&conversation, tail_size).await?;
    let req: bindings::UnlinkRequest =
        bragi::head_tail_from_bytes(&head, &tail).map_err(ServeError::Malformed)?;

    let resp = match node.unlink(req.path()).await {
        Ok(()) => bindings::SvrResponse::new(Error::Success),
        Err(e) => bindings::SvrResponse::new(e),
    };
    send_response(&conversation, &resp).await
}

async fn handle_rmdir(
    conversation: hel::Handle,
    node: Arc<dyn Node>,
    head: Vec<u8>,
    tail_size: usize,
) -> Result<(), ServeError> {
    let tail = receive_tail(&conversation, tail_size).await?;
    let req: bindings::RmdirRequest =
        bragi::head_tail_from_bytes(&head, &tail).map_err(ServeError::Malformed)?;

    let resp = match node.rmdir(req.path()).await {
        Ok(()) => bindings::SvrResponse::new(Error::Success),
        Err(e) => bindings::SvrResponse::new(e),
    };
    send_response(&conversation, &resp).await
}

async fn handle_obstruct_link(
    conversation: hel::Handle,
    node: Arc<dyn Node>,
    head: Vec<u8>,
    tail_size: usize,
) -> Result<(), ServeError> {
    let tail = receive_tail(&conversation, tail_size).await?;
    let req: bindings::ObstructLinkRequest =
        bragi::head_tail_from_bytes(&head, &tail).map_err(ServeError::Malformed)?;

    let resp = match node.obstruct_link(req.link_name()).await {
        Ok(()) => bindings::SvrResponse::new(Error::Success),
        Err(e) => bindings::SvrResponse::new(e),
    };
    send_response(&conversation, &resp).await
}

async fn handle_link(
    conversation: hel::Handle,
    head: Vec<u8>,
    tail_size: usize,
) -> Result<(), ServeError> {
    let tail = receive_tail(&conversation, tail_size).await?;
    let _req: bindings::LinkRequest =
        bragi::head_tail_from_bytes(&head, &tail).map_err(ServeError::Malformed)?;
    send_response(&conversation, &bindings::SvrResponse::new(Error::IllegalOperationTarget)).await
}

async fn handle_get_link_or_create(
    conversation: hel::Handle,
    head: Vec<u8>,
    tail_size: usize,
) -> Result<(), ServeError> {
    let tail = receive_tail(&conversation, tail_size).await?;
    let _req: bindings::GetLinkOrCreateRequest =
        bragi::head_tail_from_bytes(&head, &tail).map_err(ServeError::Malformed)?;

    let resp = bindings::GetLinkOrCreateResponse::new(
        Error::IllegalOperationTarget,
        FileType::REGULAR,
        0,
        0,
    );
    send_response(&conversation, &resp).await
}

async fn handle_utimensat(
    conversation: hel::Handle,
    _req: bindings::UtimensatRequest,
) -> Result<(), ServeError> {
    send_response(&conversation, &bindings::SvrResponse::new(Error::IllegalOperationTarget)).await
}

async fn handle_chown(
    conversation: hel::Handle,
    _req: bindings::ChownRequest,
) -> Result<(), ServeError> {
    send_response(
        &conversation,
        &bindings::ChownResponse::new(Error::IllegalOperationTarget),
    )
    .await
}

fn spawn_tailed<F, Fut>(
    head: &[u8],
    tail_size: usize,
    conversation: hel::Handle,
    handler: F,
) where
    F: FnOnce(hel::Handle, Vec<u8>, usize) -> Fut,
    Fut: Future<Output = Result<(), ServeError>> + 'static,
{
    hel::spawn(log_errors(handler(conversation, head.to_vec(), tail_size)));
}

/// Accepts a single conversation on a node lane, receives the request head and
/// dispatches it to a detached handler task.
async fn dispatch_node_request(
    lane: &hel::Handle,
    node: &Arc<dyn Node>,
) -> Result<(), ServeError> {
    let (accept, (head,)) =
        hel::submit_async(lane, hel::Accept::new((hel::ReceiveInline,))).await?;

    let conversation = match accept {
        Ok(Some(conversation)) => conversation,
        Ok(None) => return Err(ServeError::Transport(hel::Error::IllegalState)),
        Err(hel::Error::LaneShutdown | hel::Error::EndOfLane) => return Err(ServeError::Shutdown),
        Err(e) => return Err(ServeError::Transport(e)),
    };
    let head = head?;

    let preamble = bragi::preamble_from_bytes(&head).map_err(ServeError::Malformed)?;
    let tail_size = preamble.tail_size() as usize;

    match preamble.id() {
        bindings::CntRequest::MESSAGE_ID => {
            let node = node.clone();
            parse_and_spawn(&head, conversation, move |conversation, req| {
                handle_node_cnt_request(conversation, node, req)
            })?;
        }
        bindings::GetLinkRequest::MESSAGE_ID => {
            let node = node.clone();
            spawn_tailed(&head, tail_size, conversation, move |conversation, head, tail_size| {
                handle_get_link(conversation, node, head, tail_size)
            });
        }
        bindings::NodeTraverseLinksRequest::MESSAGE_ID => {
            let node = node.clone();
            spawn_tailed(&head, tail_size, conversation, move |conversation, head, tail_size| {
                handle_traverse_links(conversation, node, head, tail_size)
            });
        }
        bindings::NodeOpenRequest::MESSAGE_ID => {
            let node = node.clone();
            parse_and_spawn(&head, conversation, move |conversation, req| {
                handle_node_open(conversation, node, req)
            })?;
        }
        bindings::MkdirRequest::MESSAGE_ID => {
            let node = node.clone();
            spawn_tailed(&head, tail_size, conversation, move |conversation, head, tail_size| {
                handle_mkdir(conversation, node, head, tail_size)
            });
        }
        bindings::UnlinkRequest::MESSAGE_ID => {
            let node = node.clone();
            spawn_tailed(&head, tail_size, conversation, move |conversation, head, tail_size| {
                handle_unlink(conversation, node, head, tail_size)
            });
        }
        bindings::RmdirRequest::MESSAGE_ID => {
            let node = node.clone();
            spawn_tailed(&head, tail_size, conversation, move |conversation, head, tail_size| {
                handle_rmdir(conversation, node, head, tail_size)
            });
        }
        bindings::ObstructLinkRequest::MESSAGE_ID => {
            let node = node.clone();
            spawn_tailed(&head, tail_size, conversation, move |conversation, head, tail_size| {
                handle_obstruct_link(conversation, node, head, tail_size)
            });
        }
        bindings::LinkRequest::MESSAGE_ID => {
            spawn_tailed(&head, tail_size, conversation, handle_link);
        }
        bindings::GetLinkOrCreateRequest::MESSAGE_ID => {
            spawn_tailed(&head, tail_size, conversation, handle_get_link_or_create);
        }
        bindings::UtimensatRequest::MESSAGE_ID => {
            parse_and_spawn(&head, conversation, handle_utimensat)?;
        }
        bindings::ChownRequest::MESSAGE_ID => {
            parse_and_spawn(&head, conversation, handle_chown)?;
        }
        id => {
            eprintln!("managarm/fs: dismissing node request with unexpected message ID {id}");
            hel::spawn(log_errors(dismiss(conversation)));
        }
    }
    Ok(())
}
