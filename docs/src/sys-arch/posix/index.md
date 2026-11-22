# posix subsystem

> This part of the Handbook is Work-In-Progress.

The posix subsystem is the core of Managarm's userspace. It is started by [thor](../thoreir/index.md) and handles all posix requests made by userspace programs, like file I/O, memory allocation and sockets. It also implements various Linux API's like `epollfd`, `signalfd`, `timerfd` and `inotify`. For file I/O on block devices, it communicates with [libblockfs](../drivers/libblockfs/index.md), which is responsible for the actual file I/O on ext2 file systems.

On startup, the subsystem runs `posix-init` from the initrd. Its job is to bring up the drivers that are needed to enter the real root file system. Thus, `posix-init` does the following operations:

- Starting of the servers that are needed to reach the root file system, this includes the USB host controller drivers (`ehci`, `uhci` and `xhci`) and the block device drivers (e.g., `virtio-block`, `block-ahci`, `block-nvme` and `usb-storage`).
- Listening for uevents to bind drivers to devices that are discovered at runtime and to find the root partition. The root partition is either selected by `root=PARTUUID=` on the kernel command line or by its partition type.
- Mounting of the root (`/`) file system and entering it via `chroot`.
- Executing `systemd`, which brings up the rest of the userspace.

The code for the posix subsystem can be found at [https://github.com/managarm/managarm/tree/master/posix/subsystem](https://github.com/managarm/managarm/tree/master/posix/subsystem), while the code for `posix-init` can be found at [https://github.com/managarm/managarm/tree/master/posix/init](https://github.com/managarm/managarm/tree/master/posix/init).
