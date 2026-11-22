# servers

> This part of the Handbook is Work-In-Progress.

Servers are how most drivers running on a managarm system should be implemented. Exceptions to that are drivers that are required for kernel functionality, for instance interrupt controllers, which are part of the kernel. e9 debug logging is also implemented in-kernel. The kernel only parses the ACPI tables that it needs to set up timers and interrupts; the ACPI namespace (AML), PCI enumeration and device tree devices are handled by the sif server.

As (filesystem) servers are used to access file systems, we cannot rely on loading servers from a disk - they are loaded from the initrd for bootstrapping. This is necessary for at least all servers that can be used for file access, and their dependencies.

Other servers can be loaded from regular file systems, but they will run in the same initrd environment, which notably does not execute under the control of posix-subsystem. As these servers may rely on shared objects that are not part of the initrd, they are uploaded by `runsvr` as needed. The YAML recipes used by `bakesvr` provide the file list.

## Launching

Servers needed for bootstrapping are launched by `posix-init` (`posix/init/src/main.cpp`). After this, `posix-init` mounts the rootfs and executes systemd, which sets up file systems like procfs, sysfs, and others. Once udev is launched, our udev rules are evaluated. These use `runsvr` to bind devices to servers (via the `svrctl` protocol) or launch servers. USB udev rules are located under `drivers/usb/90-managarm-usb.rules`, and similar rules exist for NICs and graphics.
