# Trying out managarm

The Managarm project provides nightly builds of managarm if you want to try out
Managarm without setting up the development environment and building the whole
OS yourself.

## Nightly builds

The latest nightly build can be downloaded [here](https://repos.managarm.org/images/image-nightly-x86_64.zst).
Images for other architectures, as well as stable images, are available at [https://repos.managarm.org/images/](https://repos.managarm.org/images/).
These images are generated daily from the packages built by our build server.

Our build server, named `xbbs`, the xbstrap build server, which builds all of our packages automatically can be viewed at [https://builds.managarm.org](https://builds.managarm.org).

### Running the nightly builds manually

Before running the downloaded nightly build you have to **uncompress** it, for example using `zstd -d image-nightly-x86_64.zst -o image`.

To run the nightly builds we recommend using [QEMU](https://www.qemu.org/) with the following options:

```bash
qemu-system-x86_64 -enable-kvm -m 2048 -cpu host,migratable=off -device qemu-xhci -device usb-kbd -device usb-tablet -drive id=hdd,file=image,format=raw,if=none -device virtio-blk-pci,drive=hdd -vga vmware -debugcon stdio
```

Be aware that the `-enable-kvm` flags works only on systems that support KVM.

If you boot into `kmscon` (the non graphical environment), or if you issue `login` from any shell, use the following default credentials to log in.

For the root user, username: `root`, password: `toor`.
For the Managarm user, username: `managarm`. password: `managarm`.

### Running nightly builds with extra tooling

We have some extra tooling as part of our [bootstrap-managarm repo](https://github.com/managarm/bootstrap-managarm/) that may be useful to you. In order to easily obtain an image, you might want to set up a build directory, but download the image instead of building it yourself.

In order to do this, follow the setup steps from [the handbook](https://docs.managarm.org/handbook/building/index.html) up to the `xbstrap init` command. After that, you can use `xbstrap run get-nightly-image` to pull the nightly image, or `xbstrap run get-stable-image` for the stable one.

In order to run it with our default QEMU configuration, you can use `xbstrap run qemu`. If you want to use a different configuration when booting managarm, you can use our `vm-util.py` script. Feel free to explore its options:

```sh
../src/scripts/vm-util.py qemu --help
```
