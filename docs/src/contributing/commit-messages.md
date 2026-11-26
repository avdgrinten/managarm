# Commit messages

This section of the Managarm documentation describes the commit message conventions that we use.

## General

For commit messages we usually write something like this:

`<module>: <A sentence describing what was changed>`

If the intent or implementation are not immediately obvious, the commit should include an explanations of what is done. Of course, we expect to see this in any pull request too.

## Modules

We use modules to describe where in the system the change is made. For the Managarm repo, we use the following modules:
- `thor`, optionally followed by a subsystem or arch, like `thor/pci` or `thor/x86`. This is used for changes to the main kernel of Managarm, [thor](../sys-arch/thoreir/index.md).
- `eir`, optionally followed by a architecture, like `eir/x86`. This is used for changes to Managarm's prekernel, [eir](../sys-arch/thoreir/index.md).
- `posix`, used for everyting in the [posix emulation layer](../sys-arch/posix/index.md).
- `docs`, used when updating the documentation, like the one you are reading now!
- `core/<submodule>`, submodule can be either `drm` or `virtio` at the time of writing. This is used for everything related to the core of the DRM interface or virtio handling.
- `drivers/<drivername>`, where the driver name is the driver being worked on, like `drivers/libblockfs` for example. This is used for everything [driver](../sys-arch/drivers/index.md) related.
- `hel`, used for everything related to the [hel](../sys-arch/hel/index.md) and helix syscall api.
- `mbus`, used for everything related to [mbus](../sys-arch/mbus/index.md).
- `protocols/<protocol>`, where protocol is the protocol that is being changed. This is used for every protocol change made.
- `netserver`, used for every change to the network server.
- `tests`, used when working on the testsuites.
- `tools/<toolname>`, used when working on the tools that the Managarm repo has, currently `analyze-profile`, `gen-initrd`, `bakesvr` and `ostrace`.
- `utils/<utilname>`, used when updating various utilities that reside in this repo, currently `lsmbus` and `runsvr`.
- `build`, used when editing the meson.build file.
- `meta`, used for meta changes, for example a readme update.
- `.github`, used when modifying files found in the `.github` directory.

## Commit messages in the bootstrap repo

For [bootstrap-managarm](https://github.com/managarm/bootstrap-managarm), the commit message usually follows the same convention as the main repo, but there are different modules there. As we're using the Gentoo style of package separation with the file names (`sys-apps.yml` for example), we use that as a module designator too in some cases. For your use, we've listed the used modules down below:
- `<port name>`, when making changes to a port, we use the name of the port as a module specifier.
- `host-<tool name>`, when making changes to host tools used during the build.
- `<gentoo package category>`, where the category is something like `sys-apps` (see [here](https://packages.gentoo.org/categories) for more examples). This is only used when moving ports around.
- `meta`, used for meta changes, for example a readme update.
- `ci`, when making changes to the ci scripts that reside in this repo.
- `.github`, used when modifying files found in the `.github` directory.

There are two exceptions on the commit messages used in this repo, namely when adding a new port and when updating a port. When adding a new port the following message is used:

`<package name>: Add port`

And when updating a port the following message is used:

`<port name>: Update from <old version> to <new version> <optionally, if it fixes severe security vulnerabilities, list them here, for example by listing their CVE number> <optionally, if there have been other changes to the build recipe, like new configure flags, list them here>`
