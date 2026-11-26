# The managarm Operating System

![CI](https://github.com/managarm/managarm/workflows/CI/badge.svg)
[![Documentation](https://github.com/managarm/managarm/workflows/Documentation/badge.svg)](https://docs.managarm.org/handbook/)
[![x86_64](https://builds.managarm.org/projects/managarm/badge)](https://builds.managarm.org/projects/managarm)
[![aarch64](https://builds.managarm.org/projects/managarm_aarch64/badge)](https://builds.managarm.org/projects/managarm_aarch64)
[![riscv](https://builds.managarm.org/projects/managarm_riscv64/badge)](https://builds.managarm.org/projects/managarm_riscv64)

![Screenshot](../assets/screenshots/kde-epiphany.png?raw=true)

## What is this about?

This is the main repository of managarm, a microkernel-based operating system.

**What is special about managarm?** Some notable properties of managarm are:
(i) managarm is based on a microkernel while common Desktop operating systems like Linux and Windows use monolithic kernels,
(ii) managarm uses a completely asynchronous API for I/O
and (iii) despite those internal differences, managarm provides good compatibility with Linux at the user space level.

**Aren't microkernels slow?** Microkernels do have some performance disadvantages over monolithic kernels.
managarm tries to mitigate some of those issues by providing good abstractions (at the driver and system call levels)
that allow efficient implementations of common user space functionality (like POSIX).

**Is this a Linux distribution?** No, managarm runs its own kernel that does not originate from Linux.
While the managarm user space API supports many Linux APIs (e.g. epoll, timerfd, signalfd or tmpfs),
managarm does not share any source code (or binaries) with the Linux kernel.

**Official Discord server:** https://discord.gg/7WB6Ur3. This is our primary real-time communication channel.

**Official IRC channel:** `#managarm` on `irc.libera.chat`, with many more available! Type `/msg alis list managarm` to find them.

## Features

* 64-bit operating system with SMP (i.e., multicore) and ACPI support.
* Fully asynchronous microkernel and servers.
* Support for many modern hardware devices such as USB 3 controllers.
* Networking support.
* POSIX and Linux API compatibility.
* Support for Linux-style special files (epoll, signalfd, ...) and pseudo file systems (`/sys`, `/proc`, ...).

## Trying out managarm

If you want to try out managarm without building the whole OS, you can download a
zstd-compressed [nightly image](https://repos.managarm.org/images/image-nightly-x86_64.zst)
(or the [stable image](https://repos.managarm.org/images/image-stable-x86_64.zst));
these images are generated daily from the packages built by [our build server](https://builds.managarm.org)
and reflect the current state of Managarm development.
To run the (uncompressed) image using qemu, we recommend the following flags:

`qemu-system-x86_64 -display gtk,zoom-to-fit=off -enable-kvm -m 2048 -cpu host,migratable=off -device qemu-xhci -device usb-kbd -device usb-tablet -drive id=hdd,file=image,format=raw,if=none -device virtio-blk-pci,drive=hdd -vga virtio -debugcon stdio`

To log in, use the following credentials
- user: managarm
- password: managarm

## Supported Software

Programs supported on managarm include [Weston](https://gitlab.freedesktop.org/wayland/weston/) (the Wayland reference compositor), [kmscon](https://www.freedesktop.org/wiki/Software/kmscon/) (a system console), [KDE Plasma 6](https://kde.org/), GNU Coreutils, Bash, nano and others.

## Supported Hardware

**General** USB (UHCI, EHCI, XHCI)\
**Graphics** Generic VBE graphics, Intel G45, virtio GPU, Bochs VBE interface, VMWare SVGA, nVidia (Turing+)\
**Input** USB human interface devices, PS/2 keyboard and mouse\
**Storage** USB mass storage devices, NVMe, AHCI, ATA, virtio block\
**Network** USB CDC ECM/NCM ethernet devices, USB MBIM cellular modems, RTL8168 family, Intel Gigabit Ethernet devices\
**Serial** UART, CP2102 (USB to UART), FTDI FT232 (USB to UART)

## Building managarm

While this repository contains managarm's kernel, its drivers and other core functionality,
it is not enough to build a full managarm distribution. Instead, we refer to the
[Managarm Handbook](https://docs.managarm.org/handbook/building/index.html) for build instructions.

## Acknowledgements
Managarm is supported by the [NGI Zero Commons fund](https://nlnet.nl/commonsfund).

<img src="https://nlnet.nl/image/logos/NGI0CommonsFund_tag.svg" width="300" />
