/* master/uspace/pal_eoe.c */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <linux/if_ether.h>
#include <arpa/inet.h>
#include <net/if_arp.h>

#include "pal_eoe.h"
#include "../ethernet.h"

/****************************************************************************/
/* Network Device Implementation */
/****************************************************************************/

ec_netdev_t *ec_netdev_alloc(const char *name, size_t priv_size)
{
    ec_netdev_t *dev;
    
    dev = calloc(1, sizeof(*dev));
    if (!dev) {
        return NULL;
    }
    
    if (priv_size > 0) {
        dev->priv = calloc(1, priv_size);
        if (!dev->priv) {
            free(dev);
            return NULL;
        }
    }
    
    strncpy(dev->name, name, IFNAMSIZ - 1);
    dev->name[IFNAMSIZ - 1] = '\0';
    dev->fd = -1;
    dev->mtu = 1500;
    dev->opened = 0;
    dev->tx_queue_active = 0;
    
    pthread_mutex_init(&dev->tx_lock, NULL);
    
    return dev;
}

int ec_netdev_register(ec_netdev_t *dev)
{
    struct ifreq ifr;
    int fd, err;
    int sock;
    
    if (!dev) {
        return -EINVAL;
    }
    
    /* Open TUN/TAP clone device */
    fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "EoE: Failed to open /dev/net/tun: %s\n", 
                strerror(errno));
        return -errno;
    }
    
    /* Configure as TAP device (layer 2) with no packet info header */
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", dev->name);

    err = ioctl(fd, TUNSETIFF, &ifr);
    if (err < 0) {
        fprintf(stderr, "EoE: Failed to create TAP device %s: %s\n",
                dev->name, strerror(errno));
        close(fd);
        return -errno;
    }
    
    /* Update device name (kernel may have modified it) */
    strncpy(dev->name, ifr.ifr_name, IFNAMSIZ - 1);
    dev->name[IFNAMSIZ - 1] = '\0';
    dev->fd = fd;
    
    /* Get interface index */
    sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock >= 0) {
        memset(&ifr, 0, sizeof(ifr));
        snprintf(ifr.ifr_name, IFNAMSIZ, "%s", dev->name);
        if (ioctl(sock, SIOCGIFINDEX, &ifr) == 0) {
            dev->ifindex = ifr.ifr_ifindex;
        }
        close(sock);
    }
    
    fprintf(stderr, "EoE: Created TAP device %s (index %d, fd %d)\n",
            dev->name, dev->ifindex, dev->fd);
    
    return 0;
}

void ec_netdev_unregister(ec_netdev_t *dev)
{
    if (!dev) {
        return;
    }
    
    if (dev->fd >= 0) {
        close(dev->fd);
        dev->fd = -1;
        fprintf(stderr, "EoE: Destroyed TAP device %s\n", dev->name);
    }
}

void ec_netdev_free(ec_netdev_t *dev)
{
    if (!dev) {
        return;
    }
    
    /* Ensure TAP is closed */
    if (dev->fd >= 0) {
        close(dev->fd);
    }
    
    pthread_mutex_destroy(&dev->tx_lock);
    
    if (dev->priv) {
        free(dev->priv);
    }
    
    free(dev);
}

int ec_netdev_set_mac(ec_netdev_t *dev, const uint8_t mac[ETH_ALEN])
{
    struct ifreq ifr;
    int sock, ret = 0;
    
    if (!dev || dev->fd < 0) {
        return -EINVAL;
    }
    
    /* Store MAC in device structure */
    memcpy(dev->dev_addr, mac, ETH_ALEN);
    
    /* Set MAC on TAP interface */
    sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock < 0) {
        return -errno;
    }
    
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", dev->name);
    ifr.ifr_hwaddr.sa_family = ARPHRD_ETHER;
    memcpy(ifr.ifr_hwaddr.sa_data, mac, ETH_ALEN);
    
    if (ioctl(sock, SIOCSIFHWADDR, &ifr) < 0) {
        ret = -errno;
        fprintf(stderr, "EoE: Failed to set MAC on %s: %s\n",
                dev->name, strerror(errno));
    }
    
    close(sock);
    return ret;
}

/****************************************************************************/
/* Socket Buffer Implementation                                             */
/*                                                                          */
/* Frame buffers and TX frame descriptors come from a preallocated,        */
/* prefaulted and mlocked pool instead of per-frame heap allocation        */
/* (issue #175: unbounded malloc in the EoE thread). Capacity is reserved  */
/* per handler when its net_device is created (non-cyclic context): a full */
/* TX queue plus the in-flight TX frame, the RX reassembly buffer and      */
/* margin. The pool grows to the peak concurrent-handler demand and is     */
/* never shrunk — slots may still be in flight when a handler goes away.  */
/* Exhaustion (only possible while a reserve failed) drops frames          */
/* (correct for Ethernet) and counts them.                                 */
/****************************************************************************/

#define EC_SKB_POOL_BUF_SIZE 2048 /* covers ETH_FRAME_LEN and the largest
                                     EoE reassembly (63 * 32 bytes) */

typedef struct ec_skb_slot {
    ec_skb_t skb;
    struct ec_skb_slot *next; /**< Freelist link. */
    uint8_t buf[EC_SKB_POOL_BUF_SIZE];
} ec_skb_slot_t;

typedef struct ec_frame_slot {
    ec_eoe_frame_t frame;
    struct ec_frame_slot *next; /**< Freelist link. */
} ec_frame_slot_t;

static ec_skb_slot_t *skb_pool_free; /**< Buffer freelist head. */
static ec_frame_slot_t *frame_pool_free; /**< Descriptor freelist head. */
static pthread_mutex_t skb_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long skb_pool_dropped;
static unsigned int skb_pool_capacity; /**< Slots ever allocated. */
static unsigned int skb_pool_target; /**< Current per-handler demand. */

/** Slots one handler can hold at once: a full TX queue, the in-flight
 * TX frame, the RX reassembly buffer, plus margin. */
#define EC_SKB_POOL_HANDLER_SLOTS(eoe) ((eoe)->tx_queue_size + 4)

/** Grow the pool so capacity covers the new demand. Handler-creation
 * context only (allocates and mlocks). */
static int ec_skb_pool_reserve(unsigned int nslots)
{
    ec_skb_slot_t *slots;
    ec_frame_slot_t *frames;
    unsigned int add, i;

    pthread_mutex_lock(&skb_pool_lock);
    skb_pool_target += nslots;
    if (skb_pool_capacity >= skb_pool_target) {
        pthread_mutex_unlock(&skb_pool_lock);
        return 0;
    }
    add = skb_pool_target - skb_pool_capacity;
    pthread_mutex_unlock(&skb_pool_lock);

    slots = ec_rt_zalloc((size_t)add * sizeof(*slots));
    frames = ec_rt_zalloc((size_t)add * sizeof(*frames));
    if (!slots || !frames) {
        ec_rt_free(slots, (size_t)add * sizeof(*slots));
        ec_rt_free(frames, (size_t)add * sizeof(*frames));
        pthread_mutex_lock(&skb_pool_lock);
        skb_pool_target -= nslots;
        pthread_mutex_unlock(&skb_pool_lock);
        return -ENOMEM;
    }

    pthread_mutex_lock(&skb_pool_lock);
    for (i = 0; i < add; i++) {
        slots[i].next = skb_pool_free;
        skb_pool_free = &slots[i];
        frames[i].next = frame_pool_free;
        frame_pool_free = &frames[i];
    }
    skb_pool_capacity += add;
    pthread_mutex_unlock(&skb_pool_lock);
    return 0;
}

/** Give back a handler's reservation. Capacity stays allocated (slots
 * may be in flight); it is reused by the next handler. */
static void ec_skb_pool_unreserve(unsigned int nslots)
{
    pthread_mutex_lock(&skb_pool_lock);
    skb_pool_target -= nslots;
    pthread_mutex_unlock(&skb_pool_lock);
}

ec_skb_t *ec_skb_alloc(unsigned int size)
{
    ec_skb_slot_t *slot;
    unsigned long dropped = 0;

    if (size > EC_SKB_POOL_BUF_SIZE) {
        return NULL;
    }

    pthread_mutex_lock(&skb_pool_lock);
    slot = skb_pool_free;
    if (slot) {
        skb_pool_free = slot->next;
    } else {
        dropped = ++skb_pool_dropped;
    }
    pthread_mutex_unlock(&skb_pool_lock);

    if (!slot) {
        if (ec_log_ratelimit()) {
            ec_log(EC_LOG_WARNING, "EoE: frame buffer pool exhausted"
                    " (%lu frame(s) dropped)\n", dropped);
        }
        return NULL;
    }

    slot->skb.head = slot->buf;
    slot->skb.data = slot->buf;
    slot->skb.tail = slot->buf;
    slot->skb.end = slot->buf + size;
    slot->skb.len = 0;
    slot->skb.protocol = 0;
    slot->skb.dev = NULL;
    slot->skb.ip_summed = EC_CHECKSUM_NONE;

    return &slot->skb;
}

void ec_skb_free(ec_skb_t *skb)
{
    ec_skb_slot_t *slot;

    if (!skb) {
        return;
    }

    slot = (ec_skb_slot_t *) skb; /* skb is the slot's first member */
    pthread_mutex_lock(&skb_pool_lock);
    slot->next = skb_pool_free;
    skb_pool_free = slot;
    pthread_mutex_unlock(&skb_pool_lock);
}

/** Allocate a TX frame descriptor from the pool (EoE thread). */
static ec_eoe_frame_t *ec_eoe_frame_alloc(void)
{
    ec_frame_slot_t *slot;

    pthread_mutex_lock(&skb_pool_lock);
    slot = frame_pool_free;
    if (slot) {
        frame_pool_free = slot->next;
    }
    pthread_mutex_unlock(&skb_pool_lock);

    return slot ? &slot->frame : NULL;
}

/** Return a frame descriptor to the pool. Called from the shared EoE
 * code, which pairs every descriptor with exactly one pooled buffer. */
void ec_eoe_frame_free(void *frame)
{
    ec_frame_slot_t *slot = (ec_frame_slot_t *)frame; /* first member */

    if (!frame) {
        return;
    }

    pthread_mutex_lock(&skb_pool_lock);
    slot->next = frame_pool_free;
    frame_pool_free = slot;
    pthread_mutex_unlock(&skb_pool_lock);
}

uint8_t *ec_skb_put(ec_skb_t *skb, unsigned int len)
{
    uint8_t *tmp;
    
    if (!skb || skb->tail + len > skb->end) {
        return NULL;
    }
    
    tmp = skb->tail;
    skb->tail += len;
    skb->len += len;
    return tmp;
}

uint16_t ec_eth_type_trans(ec_skb_t *skb, ec_netdev_t *dev)
{
    struct ethhdr *eth;
    
    if (!skb || skb->len < ETH_HLEN) {
        return 0;
    }
    
    eth = (struct ethhdr *)skb->data;
    skb->data += ETH_HLEN;
    skb->len -= ETH_HLEN;
    skb->dev = dev;
    
    return ntohs(eth->h_proto);
}

/****************************************************************************/
/* Network RX/TX Implementation */
/****************************************************************************/

int ec_netif_rx(ec_skb_t *skb)
{
    ec_netdev_t *dev;
    ssize_t ret;
    int err = 0;

    if (!skb) {
        return -EINVAL;
    }

    /* Like the kernel's netif_rx(), this function CONSUMES the buffer
     * on every path — the caller must not free it (the delivered
     * buffers leaked before this was enforced). */

    dev = skb->dev;
    if (!dev || dev->fd < 0) {
        ec_skb_free(skb);
        return -ENODEV;
    }

    /* Write complete Ethernet frame to TAP device.
     * The frame includes the Ethernet header at skb->head.
     */
    ret = write(dev->fd, skb->head, skb->tail - skb->head);
    if (ret < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            fprintf(stderr, "EoE: TAP write error on %s: %s\n",
                    dev->name, strerror(errno));
            dev->stats.rx_errors++;
            err = -errno;
        } else {
            /* Would block - frame dropped */
            dev->stats.rx_dropped++;
            err = -EAGAIN;
        }
    } else {
        /* Update statistics for successful RX */
        dev->stats.rx_packets++;
        dev->stats.rx_bytes += ret;
    }

    ec_skb_free(skb);
    return err;
}

ec_skb_t *ec_netdev_rx_from_tap(ec_netdev_t *dev)
{
    ec_skb_t *skb;
    ssize_t len;
    
    if (!dev || dev->fd < 0) {
        return NULL;
    }
    
    /* Allocate buffer for maximum Ethernet frame */
    skb = ec_skb_alloc(ETH_FRAME_LEN);
    if (!skb) {
        return NULL;
    }
    
    /* Non-blocking read from TAP device */
    len = read(dev->fd, skb->head, ETH_FRAME_LEN);
    if (len <= 0) {
        ec_skb_free(skb);
        if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            dev->stats.tx_errors++;
        }
        return NULL;
    }
    
    skb->tail = skb->head + len;
    skb->len = len;
    skb->dev = dev;
    
    /* Update statistics for successful TX */
    dev->stats.tx_packets++;
    dev->stats.tx_bytes += len;
    
    return skb;
}

/****************************************************************************/
/* EoE Lifecycle API */
/****************************************************************************/

int ec_eoe_netdev_create(struct ec_eoe *eoe, const char *name)
{
    ec_eoe_t **priv;
    uint8_t mac_addr[ETH_ALEN] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    unsigned int pool_slots = EC_SKB_POOL_HANDLER_SLOTS(eoe);
    int ret;

    /* Reserve the handler's buffer/descriptor pool capacity up front,
     * in this non-cyclic context — not lazily at first traffic (which
     * would allocate and mlock inside the EoE thread). Without
     * buffers the handler would only drop frames, so fail creation. */
    ret = ec_skb_pool_reserve(pool_slots);
    if (ret) {
        return ret;
    }

    eoe->dev = ec_netdev_alloc(name, sizeof(ec_eoe_t *));
    if (!eoe->dev) {
        ec_skb_pool_unreserve(pool_slots);
        return -ENOMEM;
    }
    eoe->dev->pool_reserved = pool_slots;

    ec_netdev_set_mac(eoe->dev, mac_addr);

    priv = ec_netdev_priv(eoe->dev);
    *priv = eoe;

    ret = ec_netdev_register(eoe->dev);
    if (ret) {
        ec_skb_pool_unreserve(eoe->dev->pool_reserved);
        ec_netdev_free(eoe->dev);
        eoe->dev = NULL;
        return ret;
    }

    /* Make last MAC octet unique using interface index */
    mac_addr[ETH_ALEN - 1] = (uint8_t) ec_eoe_netdev_ifindex(eoe->dev);
    ec_netdev_set_mac(eoe->dev, mac_addr);

    /* In userspace, the TAP device is always "open" once created.
     * There is no ifconfig/ip-link callback, so we mark it open
     * immediately to allow the EoE state machine to process frames.
     */
    eoe->opened = 1;
    eoe->tx_queue_active = 1;

    return 0;
}

void ec_eoe_netdev_destroy(struct ec_eoe *eoe)
{
    if (eoe->dev) {
        eoe->opened = 0;
        eoe->tx_queue_active = 0;
        ec_skb_pool_unreserve(eoe->dev->pool_reserved);
        ec_netdev_unregister(eoe->dev);
        ec_netdev_free(eoe->dev);
        eoe->dev = NULL;
    }
}

/****************************************************************************/
/* EoE TX Polling                                                            */
/****************************************************************************/

/** Poll TAP device for outgoing frames and enqueue them for EoE transmission.
 *
 * In kernel space, the network stack calls ndo_start_xmit (push model).
 * In userspace, we poll the TAP fd (pull model) from the EoE thread.
 */
void ec_eoe_poll_tx(ec_eoe_t *eoe)
{
    ec_skb_t *skb;
    ec_eoe_frame_t *frame;

    if (!eoe->opened || !eoe->dev || !eoe->tx_queue_active) {
        return;
    }

    while (eoe->tx_queued_frames < eoe->tx_queue_size) {
        skb = ec_netdev_rx_from_tap(eoe->dev);
        if (!skb) {
            break;
        }

        frame = ec_eoe_frame_alloc();
        if (!frame) {
            ec_skb_free(skb);
            break;
        }

        frame->skb = skb;
        INIT_LIST_HEAD(&frame->queue);
        list_add_tail(&frame->queue, &eoe->tx_queue);
        eoe->tx_queued_frames++;
    }

    /* Stop accepting if queue is full */
    if (eoe->tx_queued_frames >= eoe->tx_queue_size) {
        ec_eoe_netdev_stop_queue(eoe->dev);
    }
}

