# Developer documentation

Design and verification documents for this tree. For the user-facing
handbook (LaTeX sources of the PDF inherited from upstream) see
[`../documentation/`](../documentation/); for build instructions see
[`../INSTALL.md`](../INSTALL.md) and for the feature list
[`../FEATURES.md`](../FEATURES.md).

## Design

| Document | What it covers |
|---|---|
| [design/userspace-master.md](design/userspace-master.md) | The userspace master library: architecture, `ecrt.h` additions, type opacity, lifecycle, build system, known limitations. |
| [design/userspace-tool.md](design/userspace-tool.md) | The `ethercat` tool over Unix-socket IPC: wire protocol, command numbers, master registry, tool-side abstraction, server side. |
| [design/pal.md](design/pal.md) | The platform abstraction layer that lets one core build as a kernel module and as a userspace library. Read this before touching anything in `master/kernel/` or `master/uspace/`. |
| [design/parallel-slave-config.md](design/parallel-slave-config.md) | Parallel Slave Configuration (PSC): per-slave FSM execution, why earlier attempts failed, limitations. |
| [design/api-usage-notes.md](design/api-usage-notes.md) | Rules of thumb for calling the API: master phases, allowed contexts, realtime constraints. |

## Testing

| Document | What it covers |
|---|---|
| [testing/rt-system-test.md](testing/rt-system-test.md) | Whole-system realtime test: system preparation, procedure, pass criteria, per-release results log. Manual, hardware-gated — the automated part lives in `make check` and CI. |

The hardware-free test suite is in [`../tests/`](../tests/); the simulated
bus that drives it is `transport/transport_sim.c`.

## History

| Document | What it covers |
|---|---|
| [history/production-readiness-review-2026-07.md](history/production-readiness-review-2026-07.md) | Frozen point-in-time audit of the userspace port (2026-07-20) and the roadmap it drove: RT design, locking, threading, security, ABI, testing, CI. Kept for the reasoning and the verification evidence. Still-open items were lifted into [`../TODO`](../TODO). |
