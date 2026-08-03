# RT System Test — Whole-System Manual Procedure

Latency and jitter are properties of the complete system: NIC, driver,
kernel configuration, CPU isolation, and the actual cyclic application.
They are therefore validated **manually on the production stack** — the
userspace master driven by LinuxCNC on a hardware test setup with the
latency test running — and not gated in CI.

CI covers the complementary *cause class* instead: blocking or
allocating code creeping into the cyclic path is caught by
`script/rt-effects-check.sh` (compile-time) and `tests/test_rt_pagefault`
(runtime page-fault delta), both environment-independent. This procedure
covers the *effect class*: actual timing on real hardware.

Record one **Test Record** (template below) per release or per relevant
change to the cyclic path, and append it to the results log at the end
of this file.

## 1. System preparation checklist

- [ ] **RLIMIT_MEMLOCK raised** (`LimitMEMLOCK=infinity` in the systemd
      unit or `memlock unlimited` in limits.conf). The default 8 MiB is
      NOT sufficient: the library's thread stacks alone exceed it, so
      `mlockall()` in the application fails — or, worse, thread creation
      inside `ecrt_lib_init()` fails if the lock is taken first.
      (Observed while building `test_rt_pagefault`.)
- [ ] Application calls `mlockall(MCL_CURRENT | MCL_FUTURE)` and
      prefaults its stack (see `examples/user/main.c`).
- [ ] Application installs a **nonblocking log callback** via
      `ecrt_lib_init()` (the stderr fallback takes a global mutex in the
      RT path — review finding F3).
- [ ] CPU isolation configured (`isolcpus=`, `irqaffinity=`, optionally
      `nohz_full=`/`rcu_nocbs=`) and the cyclic thread pinned to an
      isolated CPU. `nohz_full` is a marginal win for sleeper-based
      cyclic apps (it only suppresses the tick while a task *runs*, and
      disengages entirely when more than one task is runnable on the
      core); if used, the timer-migration item below is MANDATORY.
- [ ] **Timer migration disabled** when `nohz_full=` is set
      (`sysctl.kernel.timer_migration=0` on the kernel command line).
      With nohz_full the kernel arms all non-pinned timers — including
      the cyclic thread's `clock_nanosleep` wakeup — on a *housekeeping*
      CPU, so wakeup latency tracks the housekeeping CPU's load, which
      is exactly where the EtherCAT IRQ/NAPI/background load lives.
      (Observed 2026-07-23 on sbs: ~375 µs worst case under EtherCAT
      load, 988 ns after disabling. Diagnostic: compare per-CPU `LOC`
      rates in `/proc/interrupts` — the RT CPU must show at least
      cycle-rate local timer interrupts; a deficit there with a matching
      surplus on a housekeeping CPU means the wakeup timers migrated.)
- [ ] The master pins all of the NIC's IRQ vectors to the RT CPU
      automatically (transport `irq_pin`; log line "Pinned N transport
      IRQ(s) to CPU x"). Verify with `cat /proc/interrupts` during the
      run that the **queue-0 traffic vector** (`<iface>-TxRx-0`)
      accumulates on the RT CPU — that vector is the one that matters,
      and on igb it is NOT the lowest-numbered one (the lowest is the
      link/misc interrupt; a single-vector pin silently missed it,
      observed on sbs).
- [ ] C-states capped and cpufreq governor `performance`, via the
      kernel command line: `intel_idle.max_cstate=1
      processor.max_cstate=1 cpufreq.default_governor=performance`.
      Without a cap, deep C-states (C6-C10) add 100-400 µs exit latency
      to every cycle wakeup on an otherwise idle RT core.  Neither the
      master library nor the application is assumed to hold
      `/dev/cpu_dma_latency` — capping C-states is the deployment's
      responsibility, and this checklist item is the guard: latency
      spikes of 100-400 µs on an otherwise *idle* box mean the cmdline
      cap is missing.
- [ ] Transport choice recorded (raw / xdp-skb / xdp-native); for XDP
      note the link bounce at attach time (commit 5dab28a0).

## 2. Procedure

1. **Baseline** — run the LinuxCNC latency test on the prepared system
   *without* the EtherCAT master for ≥ 1 h. Record the histogram. If the
   baseline is already bad, fix the system before blaming the master.
2. **Soak under load** — start LinuxCNC with the master and the real
   slave chain; run a motion program that keeps the machine moving
   (e.g. an exercise g-code loop) for **≥ 24 h** with the latency
   test / servo-thread statistics running concurrently.
3. **Concurrent tool traffic** — during the soak, periodically run
   `ethercat slaves`, `ethercat sdos -p<n>` and an SDO `upload` loop, so
   the IPC server and mailbox traffic run beside the cyclic path (this
   is the production situation, not an add-on).
4. **Observables snapshots** — at start, mid-soak and end, record:
   - `ethercat master` (frame/error counters, link),
   - servo-thread max jitter and any "unexpected realtime delay" count,
   - domain working-counter state changes in the LinuxCNC log,
   - RT-thread fault counters:
     `awk '{print $10, $12}' /proc/<pid>/task/<rt-tid>/stat`
     (minor/major faults — the delta after warm-up must be zero),
   - `dmesg` for NIC/IRQ anomalies.
5. **Fault injection** — with the machine in a safe state, pull and
   replug the bus cable several times. Record recovery behavior
   (slaves back to OP, WC complete, time to recover) and that the
   machine reacts per its configured fault policy.

## 3. Pass criteria

| Criterion | Requirement |
|---|---|
| Servo thread max jitter | < 25 % of the servo period over the full soak (tighten per machine requirements) |
| Unexpected realtime delay messages | 0 |
| Datagram timeouts / corrupted / unmatched | no growth during steady state |
| Domain working counter | complete except during deliberate fault injection |
| RT-thread page faults after warm-up | 0 minor, 0 major |
| Cable-yank recovery | automatic return to OP; time recorded |

## 4. Test record template

```
Date:                YYYY-MM-DD
ethercat commit:     <hash>
linuxcnc commit:     <hash>
Rig:                 CPU / board, NIC + driver, transport, slave chain
Kernel:              version + cmdline (isolation parameters)
Cycle time:          e.g. 1 ms
Soak duration:       e.g. 26 h
Baseline max jitter: e.g. 8.2 µs
Loaded max jitter:   e.g. 11.7 µs
RT delays:           0
Datagram errors:     0 growth
Fault counters:      0 / 0
Yank recovery:       3/3 automatic, < 2 s to OP
Verdict:             PASS / FAIL (+ notes)
```

## 5. Results log

| Date | ethercat | linuxcnc | Rig | Duration | Max jitter | Verdict |
|---|---|---|---|---|---|---|
| _(no records yet)_ | | | | | | |
