# Building Managarm

This guide shows how to build a managarm distribution from source utilising the [bootstrap-managarm](https://github.com/managarm/bootstrap-managarm) patches and build scripts.

## Building a managarm distribution from source

### Build environment
To make sure that all build environments work properly, it is recommended to
setup a build environment with our lightweight containerized build runtime [cbuildrt](https://github.com/managarm/cbuildrt) (see below for instructions).
It is also possible to build with [Docker](https://www.docker.com/) 
(see [here](with-docker.md)), or by installing the dependencies manually (see [here](without-containers.md)), but these methods are no longer recommended.

Make sure that you have at least 20 - 30 GiB of free disk space.

#### Preparations

1.  Create a directory which will be used for storing the
source and build directories. Here we use `~/managarm`, but it can be any directory.
    ```sh
    mkdir ~/managarm && cd ~/managarm
    ```
1.  Install the `xbstrap` build system via `pip3`, as well as the `y4` DSL:
    ```bash
    pip3 install xbstrap y4
    ```
1.  The `git`, `subversion` and `mercurial` tools are required on the host (whether you build in a container or not). Install these via your package manager.
1.  Clone this repository into a `src` directory and create a `build` directory:
    ```bash
    git clone https://github.com/managarm/bootstrap-managarm.git src
    mkdir build
    ```

### Creating a `cbuildrt` environment

1.  Download and install the latest `cbuildrt` release by running:
    ```bash
    xbstrap prereqs cbuildrt xbps
    ```
    > Note: If you choose to build `cbuildrt` from source, make sure to place the resulting binary either in `$PATH` or in `~/.xbstrap/bin`.

    `cbuildrt` runs builds in a user namespace, which requires a range of subordinate user and group IDs for your user and the `newuidmap` and `newgidmap` programs (in the `uidmap` package on Debian and Ubuntu). Most distributions set up subordinate IDs when a user is created; if they are missing they can be added via `usermod`.

1.  **Inside the `build` directory**, create a file named `bootstrap-site.yml` with the following contents:
    ```yml
    auto_pull: true

    pkg_management:
      format: xbps

    container:
      runtime: cbuildrt
      uid: 1000
      gid: 1000
      src_mount: /var/lib/managarm-buildenv/src
      build_mount: /var/lib/managarm-buildenv/build
      allow_containerless: true
    ```
    > Note: you must keep the `src_mount` and `build_mount` values as shown above if you want to be able to use pre-built tools from our build server. These paths refer to locations on the *container*, not on your host machine. Also note that these paths cannot be changed after starting the build; doing so will likely result in a broken directory tree.
1.  If you want this build directory to be used for building for an architecture other than the default (x86_64), you can define the `arch` and `arch-triple` options. For instance, if you want to build for `riscv64`:
    ```yml
    define_options:
      arch: riscv64
      arch-triple: riscv64-managarm
      rust-arch: riscv64gc
    ```
    > Note: Every build directory only builds for the one architecture specified by the options. If you want to build managarm for multiple architectures, set up a build directory for each architecture desired.


### Building
1.  Initialize the build directory with
	```bash
	cd build
	xbstrap init ../src
	```

1.  Decide which software you want to include in the image you create. There are several meta-packages available which help you decide this:
    * `base`: just enough to boot into `kmscon` plus some functional commands to play with (such as `less` and `grep`)
    * `base-devel`: same as `base` plus some extra development tools (such as `gcc` and `binutils`)
    * `weston-desktop`: full managarm experience with a selection of terminal and GUI software

    You can install these meta-packages by doing:

    ```bash
    xbstrap install weston-desktop
    ```

    This command will download binary packages from our build servers.

1.  In order to rebuild parts of the system locally (e.g., after making changes to the source code), use the `--rebuild` flag:

	```bash
	xbstrap install --rebuild managarm-system managarm-kernel mlibc mlibc-headers
	```

### Creating Images
After managarm's packages have been built, building a HDD image of the system is straightforward. 

1.  Create the image:
	```bash
	xbstrap run initialize-empty-image
	```
1.  Copy the system onto it:
	```bash
	xbstrap run make-image # or: xbstrap run make-image-rootless
	```
    Either `make-image` or `make-image-rootless` works, with the following trade-offs:
	* `make-image` mounts the image via a loopback device. It is the fastest method, but it asks for root privileges via `sudo` (or `doas` or `su`).
	  It runs on the host instead of in the container, so the host needs `rsync`, `losetup`, `sfdisk`, `mkfs.vfat`, `mke2fs`, `e2fsck` and `debugfs`.
	* `make-image-rootless` does not need root privileges on the host. It mounts the image via `fuse2fs` inside the `cbuildrt` container, which is slower.

1.  Launch the image using QEMU:
	```bash
	xbstrap run qemu # Note that you can combine the last two operations: xbstrap run make-image qemu
	```

Alternatively, you can call the necessary scripts manually (check their help messages for more information):
* `../src/scripts/update-image.py` for copying the files onto the image (the mount method can be selected with the `--mount-using` argument)
* `../src/scripts/vm-util.py qemu` for launching QEMU
