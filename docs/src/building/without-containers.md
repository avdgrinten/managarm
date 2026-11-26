# Building without containers

This section explains how to build Managarm with manual installation of the build dependencies.

> Note: we recommend using `cbuildrt` as it provides a clean build environment
separate from your host machine, so issues are less frequent and easier to reproduce. It also allows you
to use pre-built tools from our build server rather than compiling everything from source.
> 
> Be aware that not only missing dependencies can impact the build process: some ports might incorrectly detect optional dependencies from the host operating system. Since it is infeasible to test all possible combinations of host packages, support for building outside of containers is not a priority of the project.

1.  Complete the [Preparations](index.md#preparations) section.
1.  Certain programs are required to build managarm; to get a list of the corresponding Debian packages we refer you to the `container.base_packages` list in [bootstrap.yml](https://github.com/managarm/bootstrap-managarm/blob/master/bootstrap.yml).
1.  `meson` is required. There is a Debian package, but as of Debian Stretch, a newer version is required.
    Install it from pip:
	```bash
	pip3 install meson
	```
1.  For managarm kernel documentation you may also want `mdbook`.
    You can install it from your distribution repositories, or using cargo:
	```bash
	cargo install --git https://github.com/rust-lang/mdBook.git mdbook
	```

Now proceed to the [Building](index.md#building) paragraph.
