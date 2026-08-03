# EtherCAT Master

An open-source EtherCAT master for Linux — as a **userspace shared
library**, or as the classic **kernel module**, from one shared core.

This tree is a fork of the [IgH EtherCAT
Master](https://etherlab.org/ethercat) (EtherLab), branched from upstream
`stable-1.6` at version 1.6.8. Its reason to exist is the userspace master:
the complete master core — state machines, CoE/EoE/FoE/SoE, distributed
clocks, domains — built as `libethercat.so.2` and running inside the
application process, with no kernel module and no patched network drivers.
Kernel mode is fully retained and builds from the same sources through a
platform abstraction layer.

The unmodified upstream branch is kept here as `stable-1.6` for reference
and for merging upstream fixes.

# Contents

- [What this fork adds](#what-this-fork-adds)
- [Status](#status)
- [Quick start](#quick-start)
- [Documentation](#documentation)
- [Requirements](#requirements)
- [Dry-run and field simulation](#dry-run-and-field-simulation)
- [Realtime and tuning](#realtime-and-tuning)
- [License](#license)
- [Contributing](#contributing)

# What this fork adds

- **Userspace master** (`./configure --enable-uspace-master`). The master
  core runs in the application process as a shared library; a standalone
  `ec_master` daemon covers tool-only operation without an application.
- **Pluggable transports**: raw socket (AF_PACKET, works with any
  interface), AF_XDP (SKB and native, copy mode), Beckhoff CCAT EIM via
  direct PCI BAR access, plus a public ops interface for custom transports.
- **Realtime hardening**: the cyclic path takes no locks, allocates
  nothing and issues no ioctls; priority-inheriting mutexes; library
  threads never inherit the caller's RT policy; lock-free fallback
  logging; NIC IRQ affinity pinning. `ECRT_RT_ATTR` annotations let clang's
  function-effects analysis verify the contract at compile time — in this
  tree's CI and in embedding applications.
- **Parallel Slave Configuration (PSC)**: slaves are configured
  concurrently, cutting bus startup time.
- **`ethercat` tool over IPC**: the tool talks to the userspace master
  through a Unix domain socket (mode 0660, group configurable) instead of
  a character device. Same commands, same output.
- **Hardware-free test suite and CI**: a datagram-level bus simulator
  (CoE mailbox, SDO information service, DC, FMMU/logical addressing, EoE
  echo) drives the real master core under `make check`. CI covers
  gcc/clang × ±libxdp, ASan/UBSan, ThreadSanitizer, the RT function-effects
  check, the kernel-mode build and `make distcheck`.

The full list, kernel mode included, is in [FEATURES.md](FEATURES.md).

# Status

Version 2.0.0 is the first release from this repository. The major version
reflects the architectural break, not a coordinated upstream release —
`libethercat.so.2` is deliberately ABI-distinct from the kernel-mode client
library's `.so.1` so the two can never be confused at runtime.

The userspace port has been through a full production-readiness review
(correctness, locking, threading, security, ABI, testing — see
[docs/history/](docs/history/production-readiness-review-2026-07.md)) and is
covered by the simulated-bus suite on every push. What is **not** yet on
record is a completed whole-system realtime qualification on a hardware rig;
the procedure is written down in
[docs/testing/rt-system-test.md](docs/testing/rt-system-test.md) and the
results log is still empty. Judge accordingly for your own deployment.

# Quick start

Userspace master:

```bash
./bootstrap                              # only when building from the repo
./configure --enable-uspace-master
make
sudo make install
```

```bash
sudo ec_master -i eth0 --socket-group ethercat
ethercat slaves
```

Kernel master:

```bash
./bootstrap
./configure --sysconfdir=/etc
make all modules
sudo make modules_install install
sudo depmod
```

Both procedures, including the configuration file and the udev rule, are in
[INSTALL.md](INSTALL.md). Application examples are in
[examples/](examples/).

# Documentation

- [INSTALL.md](INSTALL.md) — building and installing, both modes.
- [FEATURES.md](FEATURES.md) — feature list.
- [docs/](docs/README.md) — design and verification documents: the
  userspace master, the tool IPC protocol, the platform abstraction layer,
  parallel slave configuration, API usage rules, the RT system test.
- [`include/ecrt.h`](include/ecrt.h.in) — the application API, documented
  in place. The realtime contract for userspace applications (memory
  locking, `RLIMIT_MEMLOCK`, log callback, thread scheduling) is at
  `ecrt_lib_init()`.
- Doxygen: `git submodule update --init && make doc`.
- The upstream PDF handbook builds with `cd documentation && make`. It
  describes the kernel-mode master and does not yet cover the userspace
  port.

# Requirements

For the userspace master: a Linux system, a network interface, autotools
and a C/C++ toolchain. `libxdp`/`libbpf` are optional and enable the AF_XDP
transport. No kernel sources needed.

For the kernel master: configured sources for the running kernel. A table
of supported hardware for the native drivers is in the [upstream device
driver
list](https://docs.etherlab.org/ethercat/1.6/doxygen/devicedrivers.html).

# Dry-run and field simulation

A limited subset of the userspace API is available in `libfakeethercat`, for
running an application without a master or against emulated slaves — see
[fake_lib/README.md](fake_lib/README.md).

For testing the master itself without hardware, use the simulated transport
in `transport/transport_sim.c` as the tests in [tests/](tests/) do.

# Realtime and tuning

Realtime kernel patches are supported but not required; the realtime
processing is done by the calling application (the master code itself is
passive except for idle mode and EoE). Read the realtime notes at
`ecrt_lib_init()` in `ecrt.h` before embedding the library — in particular
the memory-locking contract and the `RLIMIT_MEMLOCK` requirement, which the
default 8 MiB limit does not satisfy.

# License

Copyright (C) 2006-2023 Florian Pose, Ingenieurgemeinschaft IgH
Copyright (C) 2026 the contributors of this fork

This is free software; you can redistribute it and/or modify it under the
terms of the GNU General Public License version 2, as published by the Free
Software Foundation.

Licensing is unchanged from upstream and follows the file headers: the
master core — and therefore the userspace library `libethercat.so.2`, which
contains it — is GPLv2 ([COPYING](COPYING)), while the kernel-mode client
library in `lib/` is LGPLv2.1 ([COPYING.LESSER](COPYING.LESSER)). Applications
embedding the userspace master link against GPLv2 code; applications using
the kernel-mode master through `lib/` do not.

It is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
details.

The EtherCAT master this is built on is the work of Florian Pose and the
IgH/EtherLab contributors; upstream development continues at
https://gitlab.com/etherlab.org/ethercat.

# Contributing

Issues and pull requests: https://github.com/stratuMAK/ethercat/issues.
Please read [CONTRIBUTING.md](CONTRIBUTING.md) and follow the coding style
in [CodingStyle.md](CodingStyle.md). Changes that fix the shared core
rather than the userspace port are welcome upstream too — keeping them
cherry-pickable onto `stable-1.6` is appreciated.
