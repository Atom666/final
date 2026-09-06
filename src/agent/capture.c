#include <agent/capture.h>
#include <agent/segment.h>
#include <agent/self_filter.h>
#include <mirror/protocol.h>
#include <mirror/util.h>

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/net_tstamp.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/ioctl.h>

static int get_offload(int fd, const char *ifname, uint32_t command, uint32_t *value) {
    struct ifreq ifr = {0}; struct ethtool_value ev = { .cmd = command };
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname); ifr.ifr_data = (void *)&ev;
    if (ioctl(fd, SIOCETHTOOL, &ifr) != 0) return -1;
    *value = ev.data; return 0;
}

void log_offload_state(const char *ifname) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0); uint32_t gro=0, gso=0, tso=0, flags=0;
    if (fd < 0 || get_offload(fd, ifname, ETHTOOL_GGRO, &gro) != 0 ||
        get_offload(fd, ifname, ETHTOOL_GGSO, &gso) != 0 || get_offload(fd, ifname, ETHTOOL_GTSO, &tso) != 0) {
        log_msg("agent", "warn", "could not query offloads for %s; leaving them unchanged", ifname);
        if (fd >= 0) close(fd);
        return;
    }
    (void)get_offload(fd, ifname, ETHTOOL_GFLAGS, &flags); close(fd);
    bool lro = (flags & ETH_FLAG_LRO) != 0;
    log_msg("agent", "info", "offload_policy=observe gro=%u gso=%u tso=%u lro=%u", gro, gso, tso, lro);
    if (gro || gso || tso || lro)
        log_msg("agent", "warn", "GRO/GSO/TSO/LRO enabled; capture may not be wire-identical; settings were not changed");
}

int capture_list_interfaces(void) {
    struct if_nameindex *interfaces = if_nameindex();
    if (!interfaces) return -1;
    for (struct if_nameindex *it = interfaces; it->if_index; it++)
        printf("%s\n", it->if_name);
    if_freenameindex(interfaces);
    return 0;
}

static void capture_vlan_meta(const struct tpacket3_hdr *tp, struct packet_item *item) {
    if (tp->tp_status & TP_STATUS_VLAN_VALID) {
        item->flags |= MIRROR_FLAG_VLAN_METADATA_PRESENT;
        item->vlan_tag_count = 1;
        item->vlan_tci = tp->hv1.tp_vlan_tci;
        item->vlan_tpid = (tp->tp_status & TP_STATUS_VLAN_TPID_VALID) ? tp->hv1.tp_vlan_tpid : 0x8100;
        if (!item->vlan_tpid) item->vlan_tpid = 0x8100;
    }
}

static void enqueue_owned_packet(struct capture_context *ctx, struct packet_item *item,
                                 uint64_t *sequence) {
    uint32_t frame_len = item->len;
    item->sequence_number = ++*sequence;
    atomic_store(&ctx->stats->sequence_number_current, *sequence);
    if (item->flags & MIRROR_FLAG_CHECKSUM_NOT_READY)
        atomic_fetch_add(&ctx->stats->checksum_not_ready_packets, 1);
    if (packet_queue_push_drop_newest(ctx->queue, item) != 0) packet_item_free(item);
    atomic_fetch_add(&ctx->stats->capture_packets, 1);
    atomic_fetch_add(&ctx->stats->capture_bytes, frame_len);
}

void *capture_thread_main(void *arg) {
    struct capture_context *ctx = arg;
    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) {
        log_msg("agent", "error", "AF_PACKET socket failed: %s", strerror(errno));
        atomic_store(&ctx->failed, 1); return NULL;
    }
    int ver = TPACKET_V3;
    if (setsockopt(fd, SOL_PACKET, PACKET_VERSION, &ver, sizeof(ver)) != 0) {
        log_msg("agent", "error", "PACKET_VERSION failed: %s", strerror(errno));
        close(fd); atomic_store(&ctx->failed, 1); return NULL;
    }
    int ifindex = if_nametoindex(ctx->cfg->capture_iface);
    if (!ifindex) {
        log_msg("agent", "error", "unknown iface %s", ctx->cfg->capture_iface);
        close(fd); atomic_store(&ctx->failed, 1); return NULL;
    }
    int interface_mtu_value = get_iface_mtu(ctx->cfg->capture_iface);
    if (interface_mtu_value < 68) {
        log_msg("agent", "error", "could not read valid MTU for %s", ctx->cfg->capture_iface);
        close(fd); atomic_store(&ctx->failed, 1); return NULL;
    }
    uint32_t interface_mtu = (uint32_t)interface_mtu_value;
    struct sockaddr_ll bind_addr = {0};
    bind_addr.sll_family = AF_PACKET;
    bind_addr.sll_protocol = htons(ETH_P_ALL);
    bind_addr.sll_ifindex = ifindex;
    if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        log_msg("agent", "error", "bind capture iface failed: %s", strerror(errno));
        close(fd); atomic_store(&ctx->failed, 1); return NULL;
    }
    struct tpacket_req3 req = {0};
    req.tp_block_size = ctx->cfg->block_size;
    req.tp_block_nr = ctx->cfg->ring_blocks;
    req.tp_frame_size = ctx->cfg->frame_size;
    req.tp_retire_blk_tov = ctx->cfg->block_timeout_ms;
    long page_size = sysconf(_SC_PAGESIZE);
    uint32_t min_frame = TPACKET_ALIGN(TPACKET3_HDRLEN + ctx->cfg->max_capture_frame_size);
    if (!req.tp_block_size || !req.tp_frame_size || !req.tp_block_nr || page_size <= 0 ||
        req.tp_block_size % (uint32_t)page_size != 0 || req.tp_block_size % req.tp_frame_size != 0 ||
        req.tp_frame_size < min_frame) {
        log_msg("agent", "error", "invalid ring geometry block=%u frame=%u blocks=%u min_frame=%u page=%ld",
                req.tp_block_size, req.tp_frame_size, req.tp_block_nr, min_frame, page_size);
        close(fd); atomic_store(&ctx->failed, 1); return NULL;
    }
    uint64_t frame_nr = (uint64_t)(req.tp_block_size / req.tp_frame_size) * req.tp_block_nr;
    if (frame_nr > UINT32_MAX) {
        log_msg("agent", "error", "capture ring frame count too large");
        close(fd); atomic_store(&ctx->failed, 1); return NULL;
    }
    req.tp_frame_nr = (uint32_t)frame_nr;
    if (setsockopt(fd, SOL_PACKET, PACKET_RX_RING, &req, sizeof(req)) != 0) {
        log_msg("agent", "error", "PACKET_RX_RING failed: %s", strerror(errno));
        close(fd); atomic_store(&ctx->failed, 1); return NULL;
    }
    size_t ring_size = (size_t)req.tp_block_size * req.tp_block_nr;
    uint8_t *ring = mmap(NULL, ring_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ring == MAP_FAILED) {
        log_msg("agent", "error", "mmap ring failed: %s", strerror(errno));
        close(fd); atomic_store(&ctx->failed, 1); return NULL;
    }
    log_msg("agent", "info", "capture started iface=%s mtu=%u blocks=%u block_size=%u frame_size=%u max_capture=%u segmentation_mode=%s",
            ctx->cfg->capture_iface, interface_mtu, req.tp_block_nr, req.tp_block_size,
            req.tp_frame_size, ctx->cfg->max_capture_frame_size,
            ctx->cfg->segmentation_mode);

    uint32_t block = 0;
    uint64_t filter_generation = 0;
    uint64_t next_packet_stats = now_ns() + 1000000000ULL;
    uint64_t seq = 0;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    while (!atomic_load(&ctx->stop)) {
        int filter_rc = self_filter_attach_socket(ctx->self_filter, fd, &filter_generation);
        if (filter_rc < 0) log_msg("agent", "warn", "could not update socket BPF filter: %s", strerror(errno));
        else if (filter_rc > 0) log_msg("agent", "info", "socket BPF self-transport filter updated");
        struct tpacket_block_desc *bd = (struct tpacket_block_desc *)(ring + (size_t)block * req.tp_block_size);
        if (!(bd->hdr.bh1.block_status & TP_STATUS_USER)) {
            poll(&pfd, 1, 250);
            continue;
        }
        uint32_t num = bd->hdr.bh1.num_pkts;
        uint32_t offset = bd->hdr.bh1.offset_to_first_pkt;
        for (uint32_t i = 0; i < num; i++) {
            struct tpacket3_hdr *tp = (struct tpacket3_hdr *)((uint8_t *)bd + offset);
            const uint8_t *pkt = (const uint8_t *)tp + tp->tp_mac;
            uint32_t snap = tp->tp_snaplen;
            uint32_t orig = tp->tp_len;
            if (snap < orig) {
                atomic_fetch_add(&ctx->stats->capture_truncated_packets, 1);
                atomic_fetch_add(&ctx->stats->capture_kernel_truncated_packets, 1);
            } else if (snap > ctx->cfg->max_capture_frame_size) {
                atomic_fetch_add(&ctx->stats->capture_truncated_packets, 1);
                atomic_fetch_add(&ctx->stats->capture_oversized_dropped_packets, 1);
            } else if (self_filter_match(ctx->self_filter, pkt, snap)) {
                atomic_fetch_add(&ctx->stats->self_transport_packets_filtered, 1);
                atomic_fetch_add(&ctx->stats->self_transport_bytes_filtered, snap);
            } else {
                struct packet_item base = {0};
                base.timestamp_ns = (uint64_t)tp->tp_sec * 1000000000ULL + tp->tp_nsec;
                struct sockaddr_ll *sll = (struct sockaddr_ll *)((uint8_t *)tp + TPACKET_ALIGN(sizeof(*tp)));
                base.packet_type = sll->sll_pkttype;
                base.direction = (sll->sll_pkttype == PACKET_OUTGOING) ? 2 : 1;
                base.flags = base.direction == 2 ? MIRROR_FLAG_TX_DIRECTION : MIRROR_FLAG_RX_DIRECTION;
                if (tp->tp_status & TP_STATUS_CSUMNOTREADY)
                    base.flags |= MIRROR_FLAG_CHECKSUM_NOT_READY;
#ifdef TP_STATUS_GSO_TCP
                if (tp->tp_status & TP_STATUS_GSO_TCP)
                    base.flags |= MIRROR_FLAG_GSO_OR_GRO_SUSPECTED;
#endif
                capture_vlan_meta(tp, &base);

                bool oversized_l3 = snap > interface_mtu + ETH_HLEN +
                                             MIRROR_MAX_VLAN_TAGS * 4u;
                bool segment_locally = strcmp(ctx->cfg->segmentation_mode, "agent") == 0;
                struct segmented_batch batch = {0};
                enum segment_result segment_rc = oversized_l3 && segment_locally ?
                    segment_tcp_frame(pkt, snap, interface_mtu, &batch) : SEGMENT_NOT_NEEDED;
                if (segment_rc == SEGMENT_OK) {
                    atomic_fetch_add(&ctx->stats->capture_segmented_aggregates, 1);
                    atomic_fetch_add(&ctx->stats->capture_generated_segments, batch.count);
                    for (size_t n = 0; n < batch.count; n++) {
                        struct packet_item item = base;
                        item.data = batch.frames[n].data;
                        item.len = batch.frames[n].len;
                        item.original_len = item.len;
                        item.flags &= ~(MIRROR_FLAG_CHECKSUM_NOT_READY |
                                        MIRROR_FLAG_GSO_OR_GRO_SUSPECTED);
                        item.flags |= MIRROR_FLAG_SOFTWARE_SEGMENTED;
                        batch.frames[n].data = NULL;
                        enqueue_owned_packet(ctx, &item, &seq);
                    }
                    segmented_batch_free(&batch);
                } else if (oversized_l3 && segment_locally) {
                    atomic_fetch_add(&ctx->stats->capture_truncated_packets, 1);
                    atomic_fetch_add(&ctx->stats->capture_oversized_dropped_packets, 1);
                    atomic_fetch_add(&ctx->stats->capture_segmentation_failures, 1);
                    if (segment_rc == SEGMENT_NO_MEMORY)
                        atomic_fetch_add(&ctx->stats->capture_alloc_failures, 1);
                } else {
                    struct packet_item item = base;
                    if (oversized_l3) {
                        item.flags |= MIRROR_FLAG_GSO_OR_GRO_SUSPECTED;
                        atomic_fetch_add(&ctx->stats->capture_forwarded_aggregates, 1);
                    }
                    item.data = malloc(snap);
                    if (item.data) {
                        memcpy(item.data, pkt, snap);
                        item.len = snap;
                        item.original_len = orig;
                        enqueue_owned_packet(ctx, &item, &seq);
                    } else atomic_fetch_add(&ctx->stats->capture_alloc_failures, 1);
                }
            }
            offset += tp->tp_next_offset;
        }
        bd->hdr.bh1.block_status = TP_STATUS_KERNEL;
        block = (block + 1) % req.tp_block_nr;
        if (now_ns() >= next_packet_stats) {
            struct tpacket_stats_v3 ps = {0}; socklen_t ps_len = sizeof(ps);
            if (getsockopt(fd, SOL_PACKET, PACKET_STATISTICS, &ps, &ps_len) == 0)
                atomic_fetch_add(&ctx->stats->kernel_ring_dropped_packets, ps.tp_drops);
            next_packet_stats = now_ns() + 1000000000ULL;
        }
    }
    munmap(ring, ring_size);
    close(fd);
    return NULL;
}
