# Parallel Slave Configuration

**Branch:** `psc`  
**Status:** Implemented and validated  
**Directory in scope:** `master/`

---

## 1. Summary

On large EtherCAT networks the startup time is dominated by slave configuration:
bringing every slave from its current AL state to the requested state (typically
OP) while writing SDO/SoE parameters, configuring PDO mappings, setting up sync
managers, DC clocks and FMMUs.  The original implementation configured slaves
**one at a time** through a single shared `ec_fsm_slave_config_t` instance
borrowing a single datagram and sub-FSMs from the master FSM.

This feature runs up to 8 slave configuration FSMs simultaneously.  Each slot
owns its own self-contained `ec_fsm_slave_config_t` with private sub-FSMs and a
**dedicated datagram**.  The master FSM directly orchestrates all slots from a
single state function.

### Results

| Setup | Sequential | Parallel | Speedup |
|-------|-----------|----------|---------|
| 5 slaves (4 simple + 1 complex) | ~4s | ~2s | 2x |
| 13 slaves (7 simple + 6 complex drives) | 23.6s | 11.6s | 2x overall, **3x config-only** |

The overall 2x is diluted by a fixed ~6s DC sync detection delay present in
both modes.  The actual configuration phase achieves 3x speedup.  The
theoretical maximum is limited by the slowest individual slave's hardware
processing time (mailbox round-trip latency cannot be reduced by parallelism).

---

## 2. Architecture

### 2.1 Design principle

**One owner, one execution context, one datagram per slot.**

Unlike the two previous failed attempts (`parallel-slave-config` and `parconf`
branches) which grafted configuration into the per-slave runtime FSM
(`ec_fsm_slave_t`) and shared/borrowed datagrams, this design keeps everything
inside `ec_fsm_master_t`:

- The master FSM owns a pool of 8 config slots
- Each slot has a **permanent dedicated datagram** (never shared)
- Each slot has a **self-contained** `ec_fsm_slave_config_t` (owns all sub-FSMs)
- The master FSM's state function directly loops all slots — no delegation

This eliminates the three failure modes that plagued the previous attempts:
1. **Datagram ownership chaos** — each slot's datagram lives for the entire
   master lifetime; no borrowing, no stale pointers
2. **State coupling** — config doesn't interact with `ec_fsm_slave_t` at all
3. **Split execution context** — single state function drives everything

### 2.2 Core data structures

**Pool slot** (defined in `master/fsm_master.h`):

```c
#define EC_FSM_SLAVE_CONFIG_POOL_SIZE 8

typedef struct {
    ec_fsm_slave_config_t fsm;      // self-contained config FSM (owns sub-FSMs)
    ec_datagram_t datagram;         // private datagram — never shared
    int in_use;                     // non-zero if slot is active
} ec_fsm_slave_config_slot_t;
```

**Pool array** (in `ec_fsm_master_t`):

```c
ec_fsm_slave_config_slot_t config_slots[EC_FSM_SLAVE_CONFIG_POOL_SIZE];
```

**Self-contained config FSM** (`master/fsm_slave_config.h`):

```c
struct ec_fsm_slave_config {
    ec_fsm_change_t fsm_change;     // owned
    ec_fsm_coe_t fsm_coe;          // owned
    ec_fsm_soe_t fsm_soe;          // owned
    ec_fsm_pdo_t fsm_pdo;          // owned
    ec_fsm_eoe_t fsm_eoe;          // owned
    ec_slave_t *slave;
    void (*state)(ec_fsm_slave_config_t *, ec_datagram_t *);
    ...
};
```

### 2.3 Execution model

The parallel configuration operates as a master FSM state
(`ec_fsm_master_state_configure_slaves`).  Each master cycle:

```
IDLE thread:  receive → exec_master_fsm → queue_datagram → send
OP thread:    (same via injection_seq handoff)
```

When `state == ec_fsm_master_state_configure_slaves`:

1. `ec_fsm_master_exec()` skips the single-datagram gate (always calls state fn)
2. `ec_fsm_master_state_configure_slaves()` loops all 8 slots:
   - Skips INIT/SENT/QUEUED datagrams (not yet ready)
   - Executes RECEIVED datagrams through their config FSM
   - When a slot finishes: frees it, immediately tries to refill with next slave
3. `ec_fsm_master_queue_datagram()` sums existing queue, queues each INIT
   slot datagram that fits within `max_queue_size`

### 2.4 Key functions

| Function | Purpose |
|----------|---------|
| `ec_fsm_master_enter_configure_slaves()` | Entry: fills up to 8 slots, sets state |
| `ec_fsm_master_state_configure_slaves()` | Per-cycle: exec all slots, refill on completion |
| `ec_fsm_master_fill_config_slot()` | Scans slaves, starts FSM in a free slot |
| `ec_fsm_master_slave_in_slot()` | Checks if slave already has an active slot |
| `ec_fsm_master_exec()` | Dispatch: skips datagram gate for config state |
| `ec_fsm_master_queue_datagram()` | Frame-fitting: queues slot datagrams within size limit |

### 2.5 Datagram propagation (D9 pattern)

At the top of `ec_fsm_slave_config_exec()`, the caller's datagram is
propagated to `fsm_change` (the only sub-FSM that stores it persistently):

```c
int ec_fsm_slave_config_exec(ec_fsm_slave_config_t *fsm, ec_datagram_t *datagram)
{
    fsm->fsm_change.datagram = datagram;

    if (datagram->state == EC_DATAGRAM_SENT
        || datagram->state == EC_DATAGRAM_QUEUED) {
        return ec_fsm_slave_config_running(fsm);
    }

    fsm->state(fsm, datagram);
    return ec_fsm_slave_config_running(fsm);
}
```

The other sub-FSMs (`fsm_coe`, `fsm_soe`, `fsm_pdo`, `fsm_eoe`) receive the
datagram as a function parameter to their `exec()` calls, so they don't need
persistent assignment.  Only `ec_fsm_change_t` stores `datagram` as a field
(its `exec()` takes no datagram parameter).

Each slot always passes `&slot->datagram` — the same dedicated datagram every
cycle.

### 2.6 Frame-fitting throttle

The queue logic respects `master->max_queue_size`:

```
max_queue_size = (send_interval_us * 1000) / EC_BYTE_TRANSMISSION_TIME_NS * 0.9
```

If a slot's datagram doesn't fit in the current frame, it stays as
`EC_DATAGRAM_INIT` and is retried next cycle.  The state function skips INIT
datagrams (they haven't been sent yet, so there's no response to process).

### 2.7 Synchronization

`config_busy` remains a boolean (set in `enter_configure_slaves`, cleared when
all slots empty).  `ec_master_enter_operation_phase()` waits on `config_queue`
until `config_busy == 0`.  No counter or pending list needed — the slot pool
handles admission directly.

---

## 3. Implementation Phases (as committed)

| Commit | Phase | Description |
|--------|-------|-------------|
| `12ea8ccb` | Phase 1 | Make `ec_fsm_slave_config_t` self-contained (owned sub-FSMs) |
| `1a239c9f` | Phase 2a | Config slot pool with sequential execution |
| `4db611f1` | Phase 2b | Per-slot private datagram |
| `e7d206f3` | Phase 2c | Frame-fitting queue logic |
| `8e135c25` | Phase 2d | Separate read-state and configure phases |
| `0b372489` | Phase 2e | Parallel execution in IDLE mode |
| `ef0da9c1` | Cleanup | Remove dead code |
| `511c7aa6` | Fix | Frame-fit safety for INIT datagrams |
| `e1055c43` | Fix | Refill slots when slaves exceed pool size |
| `45e1039f` | Cleanup | Remove debug instrumentation |

### Phase 1: Self-contained config FSM

Changed `ec_fsm_slave_config_t` from borrowing 6 pointers (datagram,
fsm_change, fsm_coe, fsm_soe, fsm_pdo, fsm_eoe) to owning all sub-FSMs as
value members.  Changed `ec_fsm_slave_config_exec()` signature to accept
`ec_datagram_t *datagram` as parameter.  Removed datagram from
`ec_fsm_change_init()`.  Added explicit datagram assignments for the master
FSM's direct `fsm_change` usage.

### Phase 2: Pool and parallel execution

Built incrementally to maintain a working system at each step:
- 2a: Pool struct, sequential execution (one slot at a time) — validates pool
- 2b: Dedicated datagram per slot — eliminates sharing
- 2c: Frame-fitting queue logic — respects bus capacity
- 2d: Separate state-read from configure — allows configure to run independently
- 2e: Execute all slots in parallel — the actual parallelism

### Bug fixes

- **Frame-fit safety**: Datagrams left as INIT (didn't fit in frame) must be
  skipped in the state function, not exec'd with stale data.
- **Slot refill**: When >8 slaves need config, freed slots must scan for the
  next unconfigured slave and start immediately (not wait for next entry).

---

## 4. Files Modified

| File | Changes |
|------|---------|
| `master/fsm_change.h` | Remove `ec_datagram_t *` from `ec_fsm_change_init()` |
| `master/fsm_change.c` | Update init; datagram set via D9 propagation |
| `master/fsm_slave_config.h` | Own sub-FSMs as values; new exec signature |
| `master/fsm_slave_config.c` | Init/clear own sub-FSMs; D9 propagation in exec |
| `master/fsm_master.h` | Pool slot type; pool array in `ec_fsm_master_t` |
| `master/fsm_master.c` | Enter/state/exec/queue functions for parallel config |
| `master/fsm_slave_scan.c` | Adapt to new `ec_fsm_slave_config_init()` signature |
| `master/master.c` | Init/clear pool slots; datagram init |

Total: **+632 -304 lines** across 8 files.

---

## 5. Why Previous Attempts Failed

Two prior branches attempted parallel slave configuration using copilot-swe-agent
(GitHub Copilot's autonomous agent):

### `parallel-slave-config` branch (6 bug-fix PRs)

| Bug | Root Cause |
|-----|-----------|
| NULL datagram segfault | Config FSM accessed datagram after `ec_fsm_slave_exec()` passed a different one |
| Datagram lifecycle bug | Response data in wrong datagram on next cycle |
| Datagram borrowing issue | Multiple FSMs sharing one datagram |
| `return` vs `continue` | Wrong control flow in slave FSM loop |
| config_running deadlock | Master FSM waiting for slave FSM that can't progress |
| Slave FSM deadlock | Pick-up condition missing config_running check |

### `parconf` branch (2 bug-fix PRs)

| Bug | Root Cause |
|-----|-----------|
| `is_ready()` bug | Config-running slaves incorrectly eligible for new requests |
| Datagram response checking | Old datagram overwritten before response consumed |

### Root cause: shared datagram and split execution context

Both approaches embedded config into `ec_fsm_slave_t` (the per-slave runtime
FSM).  This created three unsolvable problems:

1. **Datagram ownership**: `ec_fsm_slave_exec(fsm, datagram)` passes a different
   datagram each cycle.  The config FSM needs the *previous* datagram's response
   data, but it's already been overwritten.

2. **Split execution**: Config runs inside `ec_master_exec_slave_fsms()` (after
   the master FSM), so the master FSM can't observe config progress or control
   timing.

3. **State coupling**: The slave FSM handles both runtime requests (SDO/FoE) and
   config simultaneously, creating conflicts in datagram usage and state tracking.

### Why this attempt succeeded

The `psc` branch eliminates all three by keeping everything in the master FSM
with dedicated per-slot datagrams.  The 2 fixes needed (`511c7aa6`, `e1055c43`)
were corner cases in pool management — not structural design flaws.

---

## 6. Limitations and Future Work

### 6.1 DC reference clock ordering

The current implementation does not enforce that the DC reference clock slave
is configured before other DC slaves.  On tested topologies this works because
`dc_ref_time` is already set from the scan phase.  On topologies where this is
not the case, the reference clock slave should be configured first.

### 6.2 `config_changed` during parallel configuration

When `config_changed` fires, `enter_configure_slaves` detects it and restarts
from `write_system_times`.  Active slots are not aborted — they run to natural
completion.  This is safe but means a rescan waits for in-flight configs to
finish.

### 6.3 Error handling

A failed slave's `force_config` is cleared and the slot is freed.  The slave
is not retried automatically (same as sequential).  The next scan pass will
re-detect the mismatch and re-queue it.

### 6.4 Theoretical speedup limit

Parallelism overlaps the slave-side mailbox processing time.  But each SDO
round-trip still requires one bus cycle (~1ms) for the datagram.  With 6+
slaves doing SDO simultaneously, the dominant factor becomes the slowest
slave's total SDO count x per-SDO cycle time.  The speedup approaches
`sum(T_slave) / max(T_slave)` for the SDO-heavy phase.
