use std::{
    mem::{ManuallyDrop, MaybeUninit},
    ptr::NonNull,
    sync::LazyLock,
};

use hel::Handle;

bragi::include_binding!(pub mod bindings = "posix.rs");

/// Failures of requests to POSIX.
#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error(transparent)]
    Hel(#[from] hel::Error),
    /// POSIX rejected the request.
    #[error("POSIX rejected the request: {0:?}")]
    Rejected(bindings::Errors),
    /// A message could not be encoded or decoded.
    #[error("malformed message: {0}")]
    Malformed(#[from] std::io::Error),
}

pub type Result<T> = std::result::Result<T, Error>;

#[repr(i32)]
pub enum PosixSupercall {
    GetProcessData = 1,
    Fork = 2,
    Execve = 3,
    Exit = 4,
    SigKill = 5,
    SigRestore = 6,
    SigMask = 7,
    SigRaise = 8,
    Clone = 9,
    AnonAllocate = 10,
    AnonDeallocate = 11,
    SigAltStack = 12,
    SigSuspend = 13,
    GetTid = 14,
    SigGetPending = 15,
}

#[repr(C)]
struct ManagarmProcessData {
    posix_lane: hel_sys::HelHandle,
    mbus_lane: hel_sys::HelHandle,
    thread_page: NonNull<()>,
    file_table: NonNull<hel_sys::HelHandle>,
    clock_tracker_page: NonNull<()>,
    hierarchy_handle: hel_sys::HelHandle,
}

unsafe impl Send for ManagarmProcessData {}
unsafe impl Sync for ManagarmProcessData {}

static PROCESS_DATA: LazyLock<ManagarmProcessData> = LazyLock::new(|| {
    // TODO: Create wrapper in `hel`.
    let mut process_data: MaybeUninit<ManagarmProcessData> = MaybeUninit::uninit();
    let result = unsafe {
        hel_sys::helSyscall2(
            hel_sys::kHelCallSuper as i32 + PosixSupercall::GetProcessData as i32,
            process_data.as_mut_ptr() as u64,
            std::mem::size_of::<ManagarmProcessData>() as u64,
        )
    };

    if result != hel_sys::kHelErrNone as i32 {
        let error_string = unsafe { hel_sys::_helErrorString(result) };
        let error_cstr = unsafe { std::ffi::CStr::from_ptr(error_string) };

        panic!("Failed to get process data: {error_cstr:?}");
    }

    // SAFETY: If the syscall returns kHelErrNone, the process data
    // was successfully initialized by the kernel.
    unsafe { process_data.assume_init() }
});

pub fn posix_lane_handle() -> &'static Handle {
    static POSIX_LANE_HANDLE: LazyLock<ManuallyDrop<Handle>> = LazyLock::new(|| unsafe {
        ManuallyDrop::new(
            Handle::from_raw(PROCESS_DATA.posix_lane)
                .clone_handle()
                .expect("Failed to clone posix lane handle"),
        )
    });

    &POSIX_LANE_HANDLE
}

pub fn mbus_lane_handle() -> &'static Handle {
    static MBUS_LANE_HANDLE: LazyLock<ManuallyDrop<Handle>> = LazyLock::new(|| unsafe {
        ManuallyDrop::new(
            Handle::from_raw(PROCESS_DATA.mbus_lane)
                .clone_handle()
                .expect("Failed to clone mbus lane handle"),
        )
    });

    &MBUS_LANE_HANDLE
}

pub fn hierarchy_handle() -> &'static Handle {
    static HIERARCHY_HANDLE: LazyLock<ManuallyDrop<Handle>> = LazyLock::new(|| unsafe {
        ManuallyDrop::new(
            Handle::from_raw(PROCESS_DATA.hierarchy_handle)
                .clone_handle()
                .expect("Failed to clone hierarchy handle"),
        )
    });

    &HIERARCHY_HANDLE
}

/// Asks POSIX to install a file into the file table of the process with the given credentials.
/// `posix_lane` is the lane that POSIX passed to the device via OPEN_FD_LANE and `file_lane`
/// serves the fs passthrough protocol for the new file. Returns the new file descriptor.
pub async fn serve_fd(
    posix_lane: &Handle,
    credentials: [u8; 16],
    file_lane: &Handle,
) -> Result<i32> {
    let head = bragi::head_to_bytes(&bindings::ServeFdRequest::new(credentials))?;
    let (offer, (send_req, push_lane, recv_resp)) = hel::submit_async(
        posix_lane,
        hel::Offer::new((
            hel::SendBuffer::new(&head),
            hel::PushDescriptor::new(
                file_lane,
                hel_sys::kHelRightInvoke | hel_sys::kHelRightManage,
            ),
            hel::ReceiveInline,
        )),
    )
    .await?;
    offer?;
    send_req?;
    push_lane?;
    let resp: bindings::ServeFdResponse = bragi::head_from_bytes(&recv_resp?)?;
    if resp.error() != bindings::Errors::Success {
        return Err(Error::Rejected(resp.error()));
    }
    Ok(resp.fd())
}
