# Userspace Master Library Implementation

## Overview

The userspace master library (`libethercat.so.2`) embeds the full EtherCAT
master core into a shared library, so an application runs the master
in-process. It replaces — for that build — the kernel master plus ioctl
client library architecture with a single userspace library. Kernel mode is
unaffected and builds from the same core; see [pal.md](pal.md).

## Architecture

### Kernel mode

```
Application → lib/libethercat.so (ioctl client) → /dev/EtherCATN → kernel module (master core)
```

- `lib/` implements `ecrt.h` API as ioctl proxy
- `ec_master_t` in `lib/master.h` is `{ int fd; ... }` — a file descriptor wrapper
- Master core runs in kernel space

### Userspace master mode

```
Application → libethercat.so (master core + PAL + transport)
```

- `libethercat.so` contains the full master core (`master/*.c`), PAL shims, and transport layer
- `ec_master_t` is the real master struct from `master/master.h` — opaque to application via forward declaration in `ecrt.h`
- Master runs in the application's process

## API Design

### New Functions (guarded by `#ifdef EC_USPACE_MASTER` in `ecrt.h`)

```c
/** Initialize the userspace master library.
 *  Must be called once before any other ecrt_* function.
 *
 *  Internally calls:
 *    ec_log_set_callback(log_cb)
 *    ec_master_init_static()
 *    ec_pal_work_init()
 *    ec_pal_irq_work_init()
 *
 *  \param log_cb Log callback, or NULL for default (stderr).
 *  \return 0 on success, < 0 on error.
 */
EC_PUBLIC_API int ecrt_lib_init(ec_log_cb_t log_cb);

/** Start a userspace master.
 *  The caller creates transports with ec_transport_create() or
 *  ec_transport_create_by_name(), then passes them here. This function opens
 *  them on the specified interfaces, initializes the master, and enters idle
 *  phase (slave scanning starts immediately).
 *
 *  Internally calls:
 *    malloc(sizeof(ec_master_t))
 *    ec_transport_open(transport)           — interface read from transport->interface
 *    ec_transport_get_mac(transport, main_mac)    — MAC is copied
 *    if backup_transport != NULL:
 *      ec_transport_open(backup_transport)
 *      ec_transport_get_mac(backup_transport, backup_mac)
 *    ec_master_init(master, index, ..., debug_level, run_on_cpu)
 *    assign transports to device PALs (interface name read from transport->interface)
 *    ec_device_open(&master->devices[EC_DEVICE_MAIN])
 *    if backup_transport != NULL:
 *      ec_device_open(&master->devices[EC_DEVICE_BACKUP])
 *    ec_master_enter_idle_phase(master)
 *
 *  \param index        Master index (0-based).
 *  \param transport    Main transport (created with interface, not yet opened).
 *  \param backup_transport  Backup transport (created with interface, not yet
 *                      opened), or NULL.
 *  \param debug_level  Debug verbosity level.
 *  \param run_on_cpu   CPU affinity for master threads, or -1 for no binding.
 *  \return Pointer to master, or NULL on error.
 */
EC_PUBLIC_API ec_master_t *ecrt_startup_master(
        unsigned int index,
        ec_transport_t *transport,          /* main transport, required */
        ec_transport_t *backup_transport,   /* backup transport, or NULL */
        unsigned int debug_level,
        int run_on_cpu                      /* -1 = no binding */
        );

/** Cleanup the userspace master library.
 *  Any still-active masters are force-released before global infrastructure
 *  teardown.
 *
 *  Internally calls:
 *    ecrt_release_master() on all active masters
 *    ec_pal_irq_work_cleanup()
 *    ec_pal_work_cleanup()
 *
 */
EC_PUBLIC_API void ecrt_lib_cleanup(void);
```

### Existing Functions — Behavior Changes

```c
/** Release master — signature unchanged (void).
 *
 *  In uspace-master mode, additionally:
 *    ec_master_leave_idle_phase(master)
 *    ec_device_close(&master->devices[EC_DEVICE_MAIN])
 *    if backup device was opened:
 *      ec_device_close(&master->devices[EC_DEVICE_BACKUP])
 *    ec_master_clear(master)
 *    ec_transport_close(backup_transport)   — if backup was opened
 *    ec_transport_close(transport)          — always
 *    free(master)
 *    NOTE: transports are closed but NOT destroyed — caller must destroy them.
 */
EC_PUBLIC_API void ecrt_release_master(ec_master_t *master);
```

### Unchanged API (identical in both modes)

All operational functions remain identical:

- `ecrt_master_create_domain()`
- `ecrt_master_activate()` / `ecrt_master_deactivate()`
- `ecrt_master_send()` / `ecrt_master_receive()`
- `ecrt_master_application_time()`
- `ecrt_master_sync_reference_clock()` / `ecrt_master_sync_slave_clocks()`
- `ecrt_domain_*()` functions
- `ecrt_slave_config_*()` functions
- SDO, SoE, VoE, register request functions
- `ecrt_master_state()`, `ecrt_master_get_slave()`
- All other `ecrt_*` functions

### Application Code Comparison

**Kernel mode:**
```c
int main() {
    ec_master_t *master = ecrt_request_master(0);
    ec_domain_t *domain = ecrt_master_create_domain(master);
    /* ... configure slaves, activate, cyclic loop ... */
    ecrt_release_master(master);
}
```

**Userspace master mode:**
```c
int main() {
    ecrt_lib_init(NULL, EC_IPC_DEFAULT_SOCKET_PATH);

    /* Caller creates transport with interface name baked in */
    ec_transport_t *t = ec_transport_create(EC_TRANSPORT_RAW, "eth0");

    ec_master_t *master = ecrt_startup_master(
            0,               /* index */
            t,               /* main transport */
            NULL,            /* no backup transport */
            1,               /* debug_level */
            -1               /* no CPU binding */
            );

    /* FROM HERE: identical to kernel mode */
    ec_domain_t *domain = ecrt_master_create_domain(master);
    /* ... configure slaves, activate, cyclic loop ... */

    ecrt_release_master(master); /* closes transport */
    ec_transport_destroy(t);     /* caller destroys transport */

    ecrt_lib_cleanup();
}
```

## Type Opacity

`ecrt.h` already forward-declares `ec_master_t` as opaque:

```c
struct ec_master;
typedef struct ec_master ec_master_t;
```

Applications only see pointers. The full struct definition in `master/master.h`
is never included by application code. **No type renames needed.**

## Implementation Details

### Transport Ownership

The caller is responsible only for the **lifetime** of transports: create
(`ec_transport_create()`) and destroy (`ec_transport_destroy()`). The library
manages the **connection state**: `ecrt_startup_master()` opens the transports
and `ecrt_release_master()` closes them.

```
Caller:   ec_transport_create()
Library:    └─ ecrt_startup_master()  → ec_transport_open()
Library:    └─ ecrt_release_master()  → ec_transport_close()
Caller:   ec_transport_destroy()
```

`ecrt_release_master()` closes transports but does **not** destroy them —
that is the caller's responsibility.

### Backup Device Support

The caller creates a second transport and passes it along with its interface
name to `ecrt_startup_master()`. The library opens both transports and
closes both in `ecrt_release_master()`.

The `ec_master_pal_t` struct stores borrowed transport pointers:

```c
typedef struct {
    struct ec_transport *transport;        /**< Main transport (borrowed). */
    struct ec_transport *backup_transport; /**< Backup transport (borrowed, NULL if none). */
    uint8_t main_mac[ETH_ALEN];
    uint8_t backup_mac[ETH_ALEN];
} ec_master_pal_t;
```

Note: `index`, `debug_level`, and `run_on_cpu` are passed directly to
`ec_master_init()` and do not need to be stored in `ec_master_pal_t` — they
are stored in `ec_master_t` itself by `ec_master_init()`.

### MAC Address Lifetime

MAC addresses are copied into `ec_master_pal_t` at startup:

- `pal.main_mac[ETH_ALEN]` — MAC address copied from transport at startup
- `pal.backup_mac[ETH_ALEN]` — backup MAC address (zeroed if no backup)

### Master Allocation

The master is heap-allocated via `malloc(sizeof(ec_master_t))` since the
application cannot know `sizeof(ec_master_t)` — the type is opaque.
`ec_master_pal_t pal` is an embedded field of `ec_master_t` (declared in
`master/master.h`), so the PAL struct is allocated as part of the master.
This is consistent with how the existing ioctl-based `lib/common.c` allocates
masters.

### Interface Name Access

The interface name is passed to `ec_transport_create()` at creation time and
stored in `transport->interface`. `ecrt_startup_master()` calls
`ec_transport_open(transport)` which reads the interface name from
`transport->interface` and passes it to the ops `open()` call. After opening,
`transport->interface` continues to hold the name; `devices[EC_DEVICE_MAIN].name`
borrows this pointer.

### Multi-Master CLI Design

The `ec_master` tool supports multiple simultaneous masters via repeated `-i`
flags. Each `-i` starts a new master block and auto-increments the master index.

```
ec_master [-d <level>] [-f] [-l]
          -i <iface> [-t <type>] [-b <iface>] [-c <cpu>]
         [-i <iface> [-t <type>] [-b <iface>] [-c <cpu>]]
         ...
          [-h]
```

**Parameters:**

| Flag | Long option | Description |
|------|-------------|-------------|
| `-i <name>` | `--interface <name>` | Network interface — required; starts new master block, increments index |
| `-t <type>` | `--transport <type>` | Transport type for current block (default: raw) |
| `-b <name>` | `--backup <name>` | Backup interface for current block |
| `-c <id>` | `--cpu <id>` | Bind master threads to CPU for current block |
| `-d <level>` | `--debug <level>` | Debug level (global, applies to all masters) |
| `-f` | `--foreground` | Do not daemonize |
| `-l` | `--log-stdout` | Log to stdout instead of syslog (requires `--foreground`) |
| `-h` | `--help` | Show help |

**Parsing strategy:** single-pass stateful parser using `getopt_long`. Each
`-i` finalizes the previous master block and starts a new one. Block-local
options (`-t`, `-b`, `-c`) apply to the most recent `-i`. Global options
(`-d`, `-f`, `-l`) may appear anywhere.

**Example:**

```
ec_master -d 1 -i eth0 -t raw -b eth1 -c 2 -i eth2 -t xdp
```

→ master 0: main=eth0, transport=raw, backup=eth1, cpu=2, debug=1  
→ master 1: main=eth2, transport=xdp, no backup, no cpu binding, debug=1

### Daemonization

`ec_master` forks to background by default using the standard double-fork +
setsid pattern:

1. First fork: parent exits, child calls `setsid()` to become session leader
2. Second fork: session leader exits, grandchild can never acquire a terminal
3. Standard I/O redirected to `/dev/null`

`--foreground` skips all forking and keeps the process in the foreground.

A PID file is written to `/var/run/ec_master.pid` when daemonizing, and
removed on clean shutdown. This is required for init system integration and
double-start prevention.

### Syslog Support

Logging is done via a user-provided callback. `ec_log()` in `pal.c` dispatches
to the registered callback, with a stderr fallback if no callback is set.

`main.c` provides two callbacks:

- `log_to_syslog()` — uses `vsyslog()`, for daemon mode
- `log_to_stderr()` — uses `vfprintf(stderr, ...)`, for foreground mode

| Mode | Logging destination |
|------|---------------------|
| Daemon (default) | `openlog("ec_master", LOG_PID, LOG_DAEMON)` then `vsyslog()` via `log_to_syslog` callback |
| `--foreground` (without `--log-stdout`) | syslog (same as daemon mode) |
| `--foreground --log-stdout` | stderr via `log_to_stderr` callback |

`--log-stdout` requires `--foreground`: when daemonized, stdin/stdout/stderr
are redirected to `/dev/null`, so logging to stdout would silently discard
all messages.

## Build System

### Configure Option

```
--enable-uspace-master    Build userspace master library (default: no)
```

When enabled, implies:
- `--enable-kernel=no` (no kernel modules)
- `--enable-userlib=no` (no ioctl-based client library — would conflict)

### Build Targets

| Target | Source | Install |
|--------|--------|---------|
| `libethercat.so` | master core (`master/*.c`) + PAL (`master/uspace/pal*.c`) + transport (`transport/*.c`) + `master/uspace/module.c` + `master/uspace/device_uspace.c` + `master/uspace/cdev.c` + `master/uspace/tool_api.c` | `libdir` |
| `ec_master` | `master/uspace/main.c` linked against `libethercat.so` | `bindir` |

### Autotools Integration

- New `master/uspace/Makefile.am` with libtool shared library rules
- Conditional `ENABLE_USPACE_MASTER` in `master/Makefile.am`
- `AC_CONFIG_FILES` entry for `master/uspace/Makefile`
- XDP detection (`AC_CHECK_LIB` for libxdp, libbpf) when enabled

## File Changes

### New Files (implemented)

| File | Purpose |
|------|---------|
| `master/uspace/module.c` | Library lifecycle: `ecrt_lib_init()`, `ecrt_startup_master()`, `ecrt_release_master()`, `ecrt_lib_cleanup()` |
| `master/uspace/device_uspace.c` | Device functions extracted from `main.c`: `ec_device_init()`, `ec_device_clear()`, `ec_device_tx_data()`, `ec_device_send()`, `ec_device_poll()`, `ec_device_open()`, `ec_device_close()` |
| `master/uspace/cdev.c/h` | Character device emulation (IPC socket for tool API) |
| `master/uspace/tool_api.c` | Tool API implementation (responds to `ethercat` CLI commands) |
| `master/uspace/pal.c` | Logging dispatch, misc PAL functions |
| `master/uspace/pal.h` | Master PAL header (includes all sub-PAL headers) |
| `master/uspace/pal_eoe.c/h` | EoE PAL: TAP device, skb emulation, TX polling |
| `master/uspace/pal_affinity.h` | NIC IRQ CPU affinity pinning (records RT CPU, updates smp_affinity) |
| `master/uspace/pal_thread.c/h` | Thread abstraction → pthreads |
| `master/uspace/pal_work.c/h` | Work queue abstraction |
| `master/uspace/pal_irq_work.c/h` | IRQ work abstraction |
| `master/uspace/pal_alloc.h` | `ec_alloc()` → `calloc()` |
| `master/uspace/pal_list.h` | Linux-style linked list (userspace reimplementation) |
| `master/uspace/pal_mtx.h` | Mutex abstraction → `pthread_mutex_t` |
| `master/uspace/pal_sem.h` | Semaphore abstraction → `sem_t` |
| `master/uspace/pal_queue.h` | Completion queue abstraction |
| `master/uspace/pal_misc.h` | Misc utilities (jiffies, time, printk) |
| `include/ectp.h` | Transport abstraction interface (renamed from `ec_transport.h`) |
| `include/ecrt_tool.h` | Tool API header (for CLI/external tools) |
| `transport/transport.c` | Transport registry and common helpers (moved from `master/uspace/transport/`) |
| `transport/transport_raw.c` | Raw socket transport implementation (moved) |
| `transport/transport_xdp.c` | XDP/AF_XDP transport implementation (moved) |
| `transport/transport_macb.c` | MACB register-level transport |
| `transport/irq_pin.c` | NIC IRQ affinity pinning implementation |
| `master/uspace/Makefile.am` | Autotools build rules for library + binary |

### Modified Files (implemented)

| File | Change |
|------|--------|
| `include/ecrt.h` | New API functions and transport type/struct definitions under `#ifdef EC_USPACE_MASTER` |
| `master/uspace/main.c` | Creates/destroys transports; calls new `ecrt_startup_master` signature |
| `configure.ac` | Add `--enable-uspace-master` option, conditionals, implied options, XDP detection |
| `master/Makefile.am` | Conditional uspace subdirectory |

### Modified Shared Files

| File | Change |
|------|--------|
| `master/master.c` | EoE thread calls `ec_eoe_poll_tx()` before `ec_eoe_run()` |
| `master/kernel/pal_eoe.h` | No-op `ec_eoe_poll_tx()` inline for kernel build |

### Files NOT Modified

- `master/master.h` — no type renames
- `master/*.c` (other than master.c EoE hook) — master core unchanged
- `lib/` — untouched (disabled when uspace-master enabled)

## Invariants and Decisions

Properties of the implementation that are easy to break by accident.

### 1. Multiple Master Instances

The `ec_master` standalone tool is designed for multiple simultaneous masters.
Calling `ecrt_startup_master()` multiple times creates independent master
instances, each with its own `ec_master_pal_t` fields. This is intentional for
the library model (unlike the kernel module which has a global master array).

In `ec_master`, multiple masters are configured via repeated `-i` flags (see
the Multi-Master CLI Design section). Each `-i` starts a new master block with
an auto-incremented index. All masters run concurrently and are shut down
together on SIGINT/SIGTERM.

### 2. Thread Safety of `ecrt_lib_init()` / `ecrt_lib_cleanup()`

An `atomic_flag lib_initialized` guard in `master/uspace/module.c` makes
initialization idempotent. A second call to `ecrt_lib_init()` returns
`-EBUSY`. The flag is cleared on failure (so a retry is possible) and in
`ecrt_lib_cleanup()` (so re-init after cleanup works).

### 3. `ecrt_release_master()` Phase Safety

`ecrt_release_master()` starts with an `if (!master) return;` NULL guard.

The phase/active checks are correct for all normal sequences:

- If `ecrt_master_activate()` was never called, `master->active` is 0 and the
  operation-phase teardown is correctly skipped.
- If the master is in an error state where `phase` was not updated, the
  cleanup may skip necessary teardown. This matches kernel behavior.

### 4. `EC_USPACE_MASTER` Define

`include/ecrt.h` is generated from `include/ecrt.h.in` by `configure` via
`AC_CONFIG_FILES`. The substitution `@EC_USPACE_MASTER_DEFINE@` is inserted
after the header guard:

- When `--enable-uspace-master` is active: `#define EC_USPACE_MASTER 1`
- Otherwise: empty

Applications no longer need `-DEC_USPACE_MASTER` — the installed header carries
the define automatically.

### 5. Shared Library Versioning

The library uses `-version-info 2:0:0` (libtool), i.e. soname
`libethercat.so.2` — deliberately distinct from the kernel-mode ioctl
client library from `lib/` (`3:0:2`, soname `libethercat.so.1`): the two
libraries are intentionally NOT ABI compatible (different version node
sets, different lifecycle API), and distinct sonames make a runtime
mixup impossible. Follow libtool's current:revision:age rules for
future updates; exported symbols are governed by
`master/uspace/libethercat.map`.

## Known Limitations and Usage Notes

### EoE Callback Requirement

`ecrt_master_callbacks()` must be called before `ecrt_master_activate()` for
EoE (Ethernet over EtherCAT) processing to be enabled. If callbacks are not
set, EoE is effectively disabled for the master by design.

### Transport Ops Visibility (`ectp.h`)

`ec_transport_ops_t` is intentionally visible in the public `ectp.h` header so
applications can implement custom transports. Direct application use of ops
function pointers is unsupported; applications should call the
`ec_transport_*()` wrapper functions.

### `ecrt_tool.h` Stability

The tool API (`ecrt_tool_*` functions and structs referenced from
`ec_ioctl_data.h`) may change between minor releases. It is versioned
separately in the linker map (`LIBETHERCAT_USPACE_TOOL_1.0`) and should be
treated as unstable for third-party consumers.

### Multi-Master Workqueue Limitation

Multi-master userspace setups share a single global workqueue thread
(`ec_system_wq`). A work item blocked on one master (for example a slow SII
read against an unresponsive slave) can delay timeout callbacks on other
masters (for example request timeout handling and other deferred FSM work). In
practice this is typically low impact because FSM work items are designed to
stay non-blocking, but the shared resource remains a limitation.

### `ecrt_lib_init()` Idempotency

Calling `ecrt_lib_init()` more than once returns `-EBUSY` and does not
re-initialize global state (see the API documentation in `ecrt.h`). Call
`ecrt_lib_cleanup()` first if a re-initialization is really intended.
