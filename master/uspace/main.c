/*****************************************************************************
 *
 *  Copyright (C) 2006-2024  Florian Pose, Ingenieurgemeinschaft IgH
 *
 *  This file is part of the IgH EtherCAT Master.
 *
 *  The IgH EtherCAT Master is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License version 2, as
 *  published by the Free Software Foundation.
 *
 *  The IgH EtherCAT Master is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 *  Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with the IgH EtherCAT Master; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 ****************************************************************************/

/**
   \file
   Userspace EtherCAT master standalone executable main.
*/

/****************************************************************************/

#include "pal.h"

#include "../master.h"

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <getopt.h>
#include <unistd.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <grp.h>
#include <errno.h>
#include <string.h>

/****************************************************************************/

#define EC_PIDFILE      "/var/run/ec_master.pid"

/** Per-master configuration block. */
struct master_config {
    const char *interface;              /**< Main interface name. */
    const char *backup;                 /**< Backup interface name (NULL = none). */
    ec_transport_type_t transport;      /**< Transport type. */
    int cpu;                            /**< CPU affinity (-1 = no binding). */
    int irq_cpu;                        /**< CPU for the NIC IRQs (-1 = leave them). */
};

/****************************************************************************/

/** Global variable to control main loop */
static volatile int g_running = 1;

/** Global debug level */
static unsigned int g_debug_level = 1;

/** Run in foreground (do not daemonize) */
static int g_foreground = 0;

/** Log to stdout/stderr instead of syslog */
static int g_log_stdout = 0;

/** IPC socket path (NULL disables IPC server) */
static const char *g_socket_path = EC_IPC_DEFAULT_SOCKET_PATH;

/** Group granted access to the IPC socket (NULL = owner only) */
static const char *g_socket_group = NULL;

/****************************************************************************/

/** Signal handler for clean shutdown */
static void signal_handler(int signum)
{
    (void)signum;
    g_running = 0;
}

/****************************************************************************/

/** Syslog callback. */
static void log_to_syslog(int level, const char *fmt, va_list ap)
{
    vsyslog(level, fmt, ap);
}

/** Stderr log callback. */
static pthread_mutex_t stderr_log_lock = PTHREAD_MUTEX_INITIALIZER;
static const char *loglevel_names[] = {
    "EMERG", "ALERT", "CRIT", "ERR", "WARN", "NOTICE", "INFO", "DEBUG",
};

static void log_to_stderr(int level, const char *fmt, va_list ap)
{
    pthread_mutex_lock(&stderr_log_lock);
    if (level >= 0 && level <= 7)
        fprintf(stderr, "[%s] ", loglevel_names[level]);
    vfprintf(stderr, fmt, ap);
    fflush(stderr);
    pthread_mutex_unlock(&stderr_log_lock);
}

/****************************************************************************/

static void main_log(int level, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    if (g_log_stdout) {
        log_to_stderr(level, fmt, ap);
    } else {
        log_to_syslog(level, fmt, ap);
    }
    va_end(ap);
}

/****************************************************************************/

/** Double-fork daemonization. */
static int daemonize(void)
{
    pid_t pid;

    pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) _exit(0);

    if (setsid() < 0) return -1;

    pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) _exit(0);

    if (chdir("/") < 0) return -1;

    if (!freopen("/dev/null", "r", stdin)) return -1;
    if (!freopen("/dev/null", "w", stdout)) return -1;
    if (!freopen("/dev/null", "w", stderr)) return -1;

    return 0;
}

/****************************************************************************/

/** Write PID file. */
static int write_pidfile(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "%d\n", getpid());
    fclose(f);
    return 0;
}

/****************************************************************************/

/** Remove PID file. */
static void remove_pidfile(const char *path)
{
    unlink(path);
}

/****************************************************************************/

int main(int argc, char *argv[])
{
    struct master_config configs[EC_MAX_MASTERS];
    ec_master_t *masters[EC_MAX_MASTERS];
    ec_transport_t *transports[EC_MAX_MASTERS];
    ec_transport_t *backup_transports[EC_MAX_MASTERS];
    int master_count = 0;
    int current_has_interface = 0;  /* whether current block has an interface */
    int ret = 0;
    int i;
    int c;

    /* Current block defaults */
    struct master_config cur = {
        .interface = NULL,
        .backup = NULL,
        .transport = EC_TRANSPORT_RAW,
        .cpu = -1,
        .irq_cpu = -1,
    };

    static struct option long_options[] = {
        {"interface",  required_argument, 0, 'i'},
        {"transport",  required_argument, 0, 't'},
        {"backup",     required_argument, 0, 'b'},
        {"cpu",        required_argument, 0, 'c'},
        {"irq-cpu",    required_argument, 0, 'q'},
        {"debug",      required_argument, 0, 'd'},
        {"socket",       required_argument, 0, 's'},
        {"socket-group", required_argument, 0, 'g'},
        {"foreground", no_argument,       0, 'f'},
        {"log-stdout", no_argument,       0, 'l'},
        {"help",       no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    while ((c = getopt_long(argc, argv, "i:t:b:c:q:d:s:g:flh",
                    long_options, NULL)) != -1) {
        switch (c) {
            case 'i':
                /* Finalize previous block if it has an interface */
                if (current_has_interface) {
                    if (master_count >= EC_MAX_MASTERS) {
                        fprintf(stderr,
                                "Error: too many masters (max %d)\n",
                                EC_MAX_MASTERS);
                        return 1;
                    }
                    configs[master_count++] = cur;
                }
                /* Start new block with defaults */
                cur.interface = optarg;
                cur.backup = NULL;
                cur.transport = EC_TRANSPORT_RAW;
                cur.cpu = -1;
                cur.irq_cpu = -1;
                current_has_interface = 1;
                break;
            case 't':
                if (!current_has_interface) {
                    fprintf(stderr,
                            "Error: -t requires a preceding -i\n");
                    return 1;
                }
                ret = ec_transport_find_by_name(optarg);
                if (ret < 0) {
                    fprintf(stderr, "Unknown transport type: %s\n", optarg);
                    fprintf(stderr, "Available transports: ");
                    ec_transport_print_available();
                    fprintf(stderr, "\n");
                    return 1;
                }
                cur.transport = (ec_transport_type_t)ret;
                ret = 0;
                break;
            case 'b':
                if (!current_has_interface) {
                    fprintf(stderr,
                            "Error: -b requires a preceding -i\n");
                    return 1;
                }
                cur.backup = optarg;
                break;
            case 'c':
                if (!current_has_interface) {
                    fprintf(stderr,
                            "Error: -c requires a preceding -i\n");
                    return 1;
                }
                cur.cpu = (int)strtol(optarg, NULL, 0);
                break;
            case 'q':
                if (!current_has_interface) {
                    fprintf(stderr,
                            "Error: -q requires a preceding -i\n");
                    return 1;
                }
                cur.irq_cpu = (int)strtol(optarg, NULL, 0);
                break;
            case 'd':
                g_debug_level = (unsigned int)strtoul(optarg, NULL, 0);
                break;
            case 's':
                g_socket_path = optarg;
                break;
            case 'g':
                g_socket_group = optarg;
                break;
            case 'f':
                g_foreground = 1;
                break;
            case 'l':
                g_log_stdout = 1;
                break;
            case 'h':
                fprintf(stdout, "Usage: %s [OPTIONS]\n\n", argv[0]);
                fprintf(stdout,
                        "Per-master options (each -i starts a new master):\n");
                fprintf(stdout,
                        "  -i, --interface <name>    Network interface"
                        " (required, starts new master)\n");
                fprintf(stdout,
                        "  -t, --transport <type>    Transport type"
                        " (default: raw)\n");
                fprintf(stdout,
                        "  -b, --backup <name>       Backup interface\n");
                fprintf(stdout,
                        "  -c, --cpu <id>            Bind master threads"
                        " to CPU\n");
                fprintf(stdout,
                        "  -q, --irq-cpu <id>        Pin the NIC IRQs to CPU"
                        " before the first bus scan\n"
                        "                            (the CPU of the"
                        " application's cyclic thread)\n");
                fprintf(stdout, "\nGlobal options:\n");
                fprintf(stdout,
                        "  -d, --debug <level>       Set debug level"
                        " (default: 1)\n");
                fprintf(stdout,
                        "  -s, --socket <path>       IPC socket path"
                        " (default: " EC_IPC_DEFAULT_SOCKET_PATH ")\n");
                fprintf(stdout,
                        "  -g, --socket-group <grp>  Grant this group access"
                        " to the IPC socket\n");
                fprintf(stdout,
                        "  -f, --foreground          Do not daemonize\n");
                fprintf(stdout,
                        "  -l, --log-stdout          Log to stdout"
                        " (requires --foreground)\n");
                fprintf(stdout,
                        "  -h, --help                Show this help\n");
                fprintf(stdout, "\nExample:\n");
                fprintf(stdout,
                        "  %s -d 1 -i eth0 -t raw -b eth1 -c 2 -i eth2\n",
                        argv[0]);
                fprintf(stdout, "\nAvailable transports: ");
                fflush(stdout);
                ec_transport_print_available();
                fprintf(stderr, "\n");
                return 0;
            default:
                fprintf(stderr,
                        "Usage: %s -i <interface> [options]\n", argv[0]);
                return 1;
        }
    }

    /* Finalize last block */
    if (current_has_interface) {
        if (master_count >= EC_MAX_MASTERS) {
            fprintf(stderr, "Error: too many masters (max %d)\n",
                    EC_MAX_MASTERS);
            return 1;
        }
        configs[master_count++] = cur;
    }

    if (master_count == 0) {
        fprintf(stderr,
                "Error: at least one network interface required (-i option)\n");
        fprintf(stderr, "Usage: %s -i <interface> [options]\n", argv[0]);
        return 1;
    }

    if (g_log_stdout && !g_foreground) {
        fprintf(stderr,
                "Error: --log-stdout requires --foreground\n");
        return 1;
    }

    /* Resolve the socket group before daemonizing so a typo fails fast. */
    gid_t socket_gid = (gid_t)-1;
    if (g_socket_group) {
        struct group *grp = getgrnam(g_socket_group);
        if (!grp) {
            fprintf(stderr, "Error: unknown group \"%s\"\n", g_socket_group);
            return 1;
        }
        socket_gid = grp->gr_gid;
    }

    /* Set up logging */
    ec_log_cb_t log_cb = g_log_stdout ? log_to_stderr : log_to_syslog;
    if (!g_log_stdout)
        openlog("ec_master", LOG_PID, LOG_DAEMON);

    /* Daemonize unless foreground mode requested */
    if (!g_foreground) {
        if (daemonize() < 0) {
            fprintf(stderr, "Error: daemonization failed\n");
            return 1;
        }
        if (write_pidfile(EC_PIDFILE) < 0) {
            main_log(EC_LOG_WARNING, "Failed to write PID file %s\n",
                    EC_PIDFILE);
        }
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    ret = ecrt_lib_init(log_cb, g_socket_path);
    if (ret < 0) {
        main_log(EC_LOG_ERR, "Failed to initialize EtherCAT library\n");
        ret = 1;
        goto out_cleanup_log;
    }

    for (i = 0; i < master_count; i++) {
        transports[i] = NULL;
        backup_transports[i] = NULL;
        masters[i] = NULL;
    }

    /* The library creates the socket with mode 0660, owner only. Grant the
     * requested group access. Until the chown takes effect group members
     * cannot connect; non-members never can.
     * Swap hardening: pin the path with O_PATH|O_NOFOLLOW, verify the
     * pinned inode is a socket, and chown through /proc/self/fd — so a
     * symlink or file swapped in concurrently (only possible if the
     * admin pointed --socket into a world-writable directory) cannot
     * redirect the chown to an arbitrary file. */
    if (g_socket_path && socket_gid != (gid_t)-1) {
        char proc_path[64];
        struct stat st;
        int pfd = open(g_socket_path, O_PATH | O_NOFOLLOW | O_CLOEXEC);

        if (pfd < 0 || fstat(pfd, &st) < 0 || !S_ISSOCK(st.st_mode)) {
            main_log(EC_LOG_ERR, "%s is not the expected socket: %s\n",
                    g_socket_path, pfd < 0 ? strerror(errno) : "not a socket");
            if (pfd >= 0)
                close(pfd);
            ret = 1;
            goto out_release_masters;
        }
        snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", pfd);
        if (chown(proc_path, (uid_t)-1, socket_gid) < 0) {
            main_log(EC_LOG_ERR, "Failed to set group \"%s\" on %s: %s\n",
                    g_socket_group, g_socket_path, strerror(errno));
            close(pfd);
            ret = 1;
            goto out_release_masters;
        }
        close(pfd);
    }

    /* Start all masters (transports[] and backup_transports[] are already NULL) */
    for (i = 0; i < master_count; i++) {
        main_log(EC_LOG_INFO, "Starting EtherCAT master %d on interface %s"
                " (transport: %s)\n",
                i, configs[i].interface,
                ec_transport_get_name(configs[i].transport));

        /* Create main transport (not yet opened — ecrt_startup_master opens it) */
        transports[i] = ec_transport_create(configs[i].transport,
                configs[i].interface);
        if (!transports[i]) {
            main_log(EC_LOG_ERR, "Failed to create transport for master %d\n", i);
            goto out_release_masters;
        }

        /* Create backup transport if configured */
        backup_transports[i] = NULL;
        if (configs[i].backup) {
            backup_transports[i] = ec_transport_create(configs[i].transport,
                    configs[i].backup);
            if (!backup_transports[i]) {
                main_log(EC_LOG_ERR,
                        "Failed to create backup transport for master %d\n", i);
                ec_transport_destroy(transports[i]);
                transports[i] = NULL;
                goto out_release_masters;
            }
        }

        masters[i] = ecrt_startup_master(
                (unsigned int)i,
                transports[i],
                backup_transports[i],
                g_debug_level,
                configs[i].cpu,
                configs[i].irq_cpu);
        if (!masters[i]) {
            main_log(EC_LOG_ERR, "Failed to start EtherCAT master %d\n", i);
            if (backup_transports[i]) {
                ec_transport_destroy(backup_transports[i]);
                backup_transports[i] = NULL;
            }
            ec_transport_destroy(transports[i]);
            transports[i] = NULL;
            goto out_release_masters;
        }
    }

    main_log(EC_LOG_INFO, "EtherCAT master(s) started (%d total)\n",
            master_count);

    while (g_running) {
        pause();
    }

    main_log(EC_LOG_INFO, "Shutting down EtherCAT master(s)\n");
    ret = 0;

    i = master_count;
    goto cleanup_masters;

out_release_masters:
    ret = 1;
    /* i already holds the index that failed; decrement to skip already-cleaned slot */

cleanup_masters:
    while (--i >= 0) {
        if (masters[i]) {
            /* ecrt_release_master() closes the transports */
            ecrt_release_master(masters[i]);
        }
        /* destroy transports after close (or if master startup failed) */
        if (backup_transports[i]) {
            ec_transport_destroy(backup_transports[i]);
        }
        if (transports[i]) {
            ec_transport_destroy(transports[i]);
        }
    }
    ecrt_lib_cleanup();

    if (!ret)
        main_log(EC_LOG_INFO, "EtherCAT master(s) stopped\n");

out_cleanup_log:
    if (!g_foreground) {
        remove_pidfile(EC_PIDFILE);
    }
    if (!g_log_stdout) {
        closelog();
    }

    return ret;
}

