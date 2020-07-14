# 9xen

Xen paravirtualization support of Plan9 operating system

## Introduction

9Xen is a code, based on 9Front project and extended by a support of virtual displays and input events from keyboards and mice.
It works without X Window and qemu.
For the moment only x86_64 Xen and x86 Plan9 images are supported.

## Installation

### Install Xen

The code is tested on Xen 4.13.1 and 5.4.28 Linux kernel with i915 video adapter.
It would be better if debug traces are switched on.

In addition to Xen configuration of Linux kernel next options should be specified:
```
CONFIG_XEN_BLKDEV_BACKEND=y
CONFIG_XEN_NETDEV_BACKEND=y
CONFIG_DRM_XEN=y
CONFIG_XEN_DEV_EVTCHN=y
CONFIG_XEN_GNTDEV=y
CONFIG_XEN_GRANT_DEV_ALLOC=y
```

### Install DRM backend.

xen_troops/displ_be <https://github.com/xen-troops/displ_be> is only DRM backend exists for the moment.
It should be configured without `ZCOPY`, because it does not work for Xen x86_64:
```
cmake ${PATH_TO_SOURCES} -WITH_DRM=ON -DWITH_INPUT=ON -DWITH_ZCOPY=OFF -DWITH_WAYLAND=OFF
```

### Build Plan9 Xen image (optional).

Just copy the content of /sys directory to your Plan9 distribution and build usual way.

## Deploying

deploy directory contains an already compiled x86 Plan9 Xen image.
Also there is a not very smart script that generates Xen configs.
It takes a first varians of displays, mice and keyboards it has found and use them in the Xen configuration.
*.sample configs contains real configuration for Acer Travelmate notebook with Gentoo Linux.
If a network support is needed, it should be configured according to a way for the current Linux distribution.
The network support works at least for the bridge configuration.

## Additional configuration.

### Resolution and colour scheme.

Resolution is obtained from vdispl configuration. Colour scheme can be specified in the extra configuration by specification of `fourcc` option.
Next `FOURCC` sequences are supported:


| FOURCC | Plan9 channel |
| --- | --- |
| `RG16` | `RGB16` |
| `BG24` | `BGR24` |
| `RA24` | `RGBA32` |
| `AR24` | `ARGB32` |
| `XR24` | `XRGB32` |
| `AB24` | `ABGR32` |
| `XBA8` | `XBGR32` |


Default is `RG16`.

Your DRM driver can limit a support only a subset of the codes.

### Stride.

There is a problem with DRM drivers - they can calculate a wrong size of line of pixels (stride).
For non-ZCOPY variants the workaround is supported in xen-troops/displ_be.
But if a separated domain with DRM is supported, it can be configured with ZCOPY support and for the case a stride can be specified in the Xen configuration.
Specify a size of stride in `stride` option of the extra configuration.

## Using

Start displ_be:
```
/usr/local/bin/displ_be -m DRM
```

For the first time start installation of Plan9 by
```
xl create -c 9xen_install
```

Install the system by
```
inst/start
```
in a terminal window.

After the installation leave the system by
```
halt -r
```
or
```
reboot
```

Start the installed system by
```
xl create -c 9xen
```

## Bugs

Sometimes an entire system hangs by unknown reason until hard reset. Potentially the separated DRM domain can help with the problem, at least it can be easy restarted without hard reset of the entire system.
