# Platform Abstraction Layer (PAL)

How the shared master core is kept free of platform API, so that the same
sources build both as a kernel module and as a userspace shared library.

## Principle

```
master/                shared core — no direct kernel or POSIX API
master/kernel/         kernel-mode platform implementation (kernel API ok)
master/uspace/         userspace platform implementation (POSIX API ok)
```

Shared code calls neutral, `ec_`-prefixed PAL functions only. Each platform
provides an implementation of that API — the kernel side as thin inline
wrappers around the real kernel API, the userspace side as pthread/POSIX
implementations. The rule is deliberately one-directional: **the PAL never
mimics kernel names.** An earlier iteration had `master/uspace/` define
`sema_init()`, `kthread_run()`, `wake_up()` and friends so that shared code
could keep calling kernel names; that inverted the dependency and leaked the
kernel namespace into every translation unit. Shared code now names
`ec_sem_init()`, and both sides implement it.

Each side has a single `pal.h` aggregating the per-concept headers, included
by the shared core. The two trees are kept structurally symmetric — a
concept has the same file name on both sides.

## Subsystems

| Header | Shared API | Kernel maps to | Userspace maps to |
|---|---|---|---|
| `pal_sem.h` | `ec_sem_init/down/up/down_interruptible/down_trylock` | `struct semaphore` | pthread mutex, `PTHREAD_PRIO_INHERIT` + `PTHREAD_MUTEX_ERRORCHECK` |
| `pal_mtx.h` | `ec_mutex_init/lock/unlock/destroy`, `ec_rt_lock_interruptible` | `rt_mutex_*` | pthread mutex |
| `pal_queue.h` | `ec_wq_init/wake/wake_all/wake_interruptible/wait/wait_interruptible/wait_timeout` | `wait_queue_head_t` | condition variable |
| `pal_thread.h` | `ec_thread_run/stop/should_stop/yield/yield_timeout/wake/detach/set_priority/bind_cpu` | kthread API | pthreads with explicit scheduling attributes |
| `pal_work.h` | `ec_work_init/schedule/cancel`, `ec_pal_work_init/cleanup` | workqueues | worker thread |
| `pal_irq_work.h` | `ec_irq_work_init/queue/sync`, `ec_pal_irq_work_init/cleanup` | `irq_work` | deferred-work thread |
| `pal_alloc.h` | `ec_alloc/zalloc/free/valloc/vfree/alloc_atomic`, `ec_rt_alloc(_aligned)/rt_zalloc/rt_free`, `ec_rt_lock_mem/unlock_mem` | `kmalloc`/`vmalloc` | `malloc`/`posix_memalign` + `mlock` |
| `pal_misc.h` | `ec_log`, `ec_log_ratelimit`, `ec_log_set_callback`, `ec_pal_log_start/stop` | `printk` | application callback, or log ring + drainer thread |
| `pal_eoe.h` | `ec_eoe_netdev_*`, `ec_eoe_buf_*` | `net_device` / `sk_buff` | TAP device / pooled frame buffers |
| `pal_affinity.h` | `ec_pal_record_rt_cpu`, `ec_pal_check_irq_affinity`, `ec_pal_check_link_states`, `ec_transport_set_cpu_affinity` | mostly no-ops (the NIC driver owns this) | `sched_getcpu`, IRQ affinity check, transport link poll |
| `pal_list.h` (uspace only) | kernel-style intrusive list | `<linux/list.h>` | vendored copy — a data structure, not a platform API |

The device layer follows the same split: `ec_device_pal_t` holds the
`net_device`/`module` pair in kernel mode and the transport handle plus link
state in userspace mode, while the common part of device init lives in
shared `master/device.c`.

## EoE

`master/ethernet.c` is shared code that was originally 100% kernel code, so
EoE needed the deepest split. It is done in three layers:

- **Types.** `ec_eoe_netdev_t`, `ec_eoe_buf_t` and `ec_eoe_stats_t` are
  opaque on both sides, so `ethernet.h` names no kernel type. Field access
  goes through accessors (`ec_eoe_netdev_name()`, `ec_eoe_netdev_ifindex()`).
- **Lifecycle and callbacks.** `ec_eoe_netdev_create()`/`_destroy()` and the
  queue operations (`tx_lock`, `tx_unlock`, `start_queue`, `stop_queue`,
  `wake_queue`) hide `alloc_netdev`/`register_netdev`/`netif_*`. The
  `ndo_open`/`ndo_stop`/`ndo_start_xmit`/`ndo_get_stats` callbacks live in
  `master/kernel/pal_eoe.c`, not in shared code.
- **Buffers.** `ec_eoe_buf_alloc/free/put/len/data` plus the RX-completion
  wrappers (`set_dev`, `set_protocol`, `set_checksum`, `deliver`) replace all
  `skb_*` use. In userspace the buffers come from a preallocated, mlocked
  per-handler pool, so the EoE thread performs no allocation in steady state.

## Cross-thread state: `EC_PAL_SHARED`

`master/master_globals.h` defines `EC_PAL_SHARED` as `_Atomic` in the
userspace build and as nothing in the kernel build. Variables handed over
between the cyclic application thread and the master/FSM/EoE threads carry
it, which turns the plain assignments and comparisons already present in the
shared core into sequentially consistent atomics without touching call sites.
That is what makes the TSan CI job meaningful — see the
[production-readiness review](../history/production-readiness-review-2026-07.md)
for the conversion and the ordering bug it exposed.

Do not add a new lock-free cross-thread variable without `EC_PAL_SHARED`.

## Realtime annotations

Functions reachable from the application's cyclic path carry `EC_RT_ATTR`
(internal, `globals.h`) or `ECRT_RT_ATTR` (public, `include/ecrt_rt.h`),
which expand to `__attribute__((nonblocking))` on clang ≥ 20 and to nothing
elsewhere. Deliberate nonblocking-by-flag leaves are wrapped in
`EC_RT_TRUSTED_BEGIN/END` with a justification comment; audit them with
`grep -rn RT_TRUSTED master/ transport/`. `script/rt-effects-check.sh`
enforces the contract in CI, so a blocking call added to the cyclic path
fails the build. See [api-usage-notes.md](api-usage-notes.md).

## Adding a PAL call

1. Name it `ec_<subsystem>_<verb>` and declare it in the shared code that
   needs it — never a kernel or POSIX name.
2. Implement it in `master/kernel/pal_<subsystem>.h` (thin inline wrapper)
   and `master/uspace/pal_<subsystem>.{h,c}`.
3. If it can be reached from the cyclic path, annotate it `EC_RT_ATTR` and
   run `script/rt-effects-check.sh`.
4. Build both modes. Kernel mode is not optional: CI's `build-kernel` job
   compiles it against the distro headers, and `make distcheck` verifies that
   the headers actually ship.

## History

The migration was carried out in six tracked priorities — neutral-name
rename, EoE split, bug fixes, device init/clear split, kernel-side balance,
cleanup — and every item was closed before 2.0.0. This file used to be that
checklist; it is preserved in the git history of `PAL_IMPLEMENTATION.md`.
