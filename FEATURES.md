This is the FEATURES file of the IgH EtherCAT Master.

vim: spelllang=en spell

# General Features

- EtherCAT master implementation conforming to IEC/PAS 62407.
  - Runs as kernel module, or entirely in userspace as a shared library
    (see "Userspace Master" below).
  - Multiple masters possible on one machine.

- Native EtherCAT-capable versions of standard Linux drivers for wide-spread
  Ethernet devices, as well as a generic driver for all chips supported by the
  Linux kernel.
  - Interrupt-less operation of Ethernet devices when using native drivers.
  - Easy implementation of additional native Ethernet drivers through common
    device interface.
  - Operation possible with any device supported by the standard drivers,
    including PCMCIA devices.
  - For any other hardware, the generic driver can be used. It uses the lower
    layers of the Linux network stack.

- Supports any realtime environment through independent architecture.
  - RTAI, Xenomai, RT-Preempt, etc.
  - RTDM Interface for userspace realtime environments
  - Operation possible without any realtime extension at all.

# Userspace Master (--enable-uspace-master)

- The complete master core (FSMs, CoE/EoE/FoE/SoE, DC, domains) runs
  inside the application process as a shared library (`libethercat.so.2`)
  — no kernel module, no patched NIC drivers.
  - Kernel mode is fully retained; both modes share the same core through
    a platform abstraction layer (PAL, see docs/design/pal.md).
  - Standalone `ec_master` daemon for tool-only operation without an
    application.
- Pluggable userspace transports (`ectp.h`):
  - Raw socket (AF_PACKET) — works with any network interface.
  - AF_XDP (SKB and native mode) for reduced latency. Both use
    XDP copy mode for universal driver compatibility (true
    zero-copy is a possible future optimization).
  - Beckhoff CCAT EIM via direct PCI BAR access (no kernel module).
  - Public transport ops interface for custom transports.
- Realtime-friendly by construction:
  - The cyclic path (receive/process/queue/send) takes no locks,
    performs no allocations and issues no ioctls; RT-critical buffers
    are prefaulted and mlocked.
  - Priority-inheriting mutexes; library threads never inherit the
    caller's realtime policy (configurable via
    `ecrt_lib_set_thread_scheduling()`).
  - Lock-free fallback logging from realtime contexts.
  - `ECRT_RT_ATTR` function annotations for compile-time RT verification
    of application code (clang function-effects analysis).
  - Automatic NIC IRQ affinity pinning to the realtime CPU.
- Parallel Slave Configuration (PSC): slaves are configured
  concurrently, reducing bus startup time.
- `ethercat` command-line tool connects over a Unix domain socket
  (mode 0660; group configurable via `ec_master --socket-group`).
- Hardware-free test suite: a simulated bus (datagram-level slave
  emulation including CoE mailbox, SDO Information Service, DC and
  FMMU/logical addressing) drives the real master core in `make check`;
  CI covers gcc/clang, ASan/UBSan, ThreadSanitizer (race-free baseline)
  and the kernel-mode build.

- Common API for Realtime-Applications in kernel- and userspace.
  - Requesting and releasing masters.
  - Dynamic slave configuration, even for slaves that are offline.
  - Detailed configuration of the slaves' PDOs and SDOs.
  - Creation of process data domains (see below). Registration of PDO entries
    for exchange within a domain.
  - Monitoring the states of masters, slave configurations and domains.
  - SDO handlers for application-triggered CoE transfers (see below).
  - VoE handlers for Vendor-specific mailbox protocols (see below).
  - Similar userspace implementation of the kernel API via a C-library.
  - Avoidance of unnecessary copy operations for process data.

- Separating slave groups through domains.
  - Handling of multiple slave groups with different sampling rates.
  - Automatic calculation of process data mapping, FMMU- and sync manager
    configuration within the domains.
  - Process data exchange can be monitored via a per-domain mechanism.

- Master finite state machine (FSM).
  - The same state machine runs both in idle mode and in realtime operation.
  - Bus monitoring: Slave states are read cyclically. Automatic scanning of the
    bus after a topology change.
  - Automatic configuration of slaves, if a application-layer state change is
    requested.

- Implementation of the "CANopen over EtherCAT" (CoE) mailbox protocol.
  - Configuration of CoE-capable slaves.
  - SDO information service (dictionary listing).
  - SDO transfers both via the application interface and the command-line tool.

- Implementation of the "Ethernet over EtherCAT" (EoE) mailbox protocol.
  - Virtual network interface for any EoE-capable slave.
  - Both a switched and a routed EoE network architecture is natively supported
    and configurable with standard tools.

- Implementation of the "Vendor-specific over EtherCAT" (VoE) mailbox protocol.
  - Communication with vendor-specific mailbox protocols via the API.

- Implementation of the "File Access over EtherCAT" (FoE) mailbox protocol.
  - Loading and storing files via the command-line tool.
  - Updating a slave's firmware can be done easily.

- Userspace command-line tool `ethercat`.
  - Detailed information about master, slaves, domains and bus configuration.
  - Setting the master's debug level.
  - Reading/Writing alias addresses.
  - Listing slave configurations.
  - Viewing process data.
  - SDO download/upload; listing SDO dictionaries.
  - Loading and storing files via FoE.
  - Access to slave registers.
  - Slave SII (EEPROM) access.
  - Controlling application-layer states.
  - Generation of slave description XML from existing slaves.

- Seamless integration in any GNU/Linux distribution.
  - "Linux Standard Base"-compatible init script for master control.
  - Master and Ethernet device configuration via sysconfig file.

- Virtual read-only network interface for debugging and traffic monitoring
  purposes (using Wireshark, etc.). No additional hardware necessary.
