#include <agent/capture.h>
#include <agent/segment.h>
#include <agent/self_filter.h>
#include <mirror/net.h>
#include <mirror/protocol.h>
#include <mirror/util.h>

#include <pcap.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void enqueue_owned_packet(struct capture_context *ctx,
                                 struct packet_item *item,
                                 uint64_t *sequence) {
    uint32_t frame_len = item->len;
    item->sequence_number = ++*sequence;
    atomic_store(&ctx->stats->sequence_number_current, *sequence);
    if (item->flags & MIRROR_FLAG_CHECKSUM_NOT_READY)
        atomic_fetch_add(&ctx->stats->checksum_not_ready_packets, 1);
    if (packet_queue_push_drop_newest(ctx->queue, item) != 0)
        packet_item_free(item);
    atomic_fetch_add(&ctx->stats->capture_packets, 1);
    atomic_fetch_add(&ctx->stats->capture_bytes, frame_len);
}

static int resolve_device(const char *configured, char *resolved,
                          size_t resolved_len, char *description,
                          size_t description_len) {
    char errbuf[PCAP_ERRBUF_SIZE];
    if (pcap_init(PCAP_CHAR_ENC_UTF_8, errbuf) != 0) {
        log_msg("agent", "error", "Npcap initialization failed: %s", errbuf);
        return -1;
    }
    pcap_if_t *devices = NULL;
    if (pcap_findalldevs(&devices, errbuf) != 0) {
        log_msg("agent", "error", "Npcap device enumeration failed: %s", errbuf);
        return -1;
    }
    pcap_if_t *match = NULL;
    for (pcap_if_t *it = devices; it; it = it->next) {
        if (!strcmp(configured, it->name) ||
            (it->description && !strcmp(configured, it->description))) {
            if (match) {
                log_msg("agent", "error", "ambiguous Npcap adapter '%s'; use device name",
                        configured);
                pcap_freealldevs(devices);
                return -1;
            }
            match = it;
        }
    }
    if (!match) {
        log_msg("agent", "error", "Npcap adapter '%s' not found; run --list-interfaces",
                configured);
        pcap_freealldevs(devices);
        return -1;
    }
    snprintf(resolved, resolved_len, "%s", match->name);
    snprintf(description, description_len, "%s",
             match->description ? match->description : "");
    pcap_freealldevs(devices);
    return 0;
}

int capture_list_interfaces(void) {
    char errbuf[PCAP_ERRBUF_SIZE];
    if (pcap_init(PCAP_CHAR_ENC_UTF_8, errbuf) != 0) {
        fprintf(stderr, "Npcap initialization failed: %s\n", errbuf);
        return -1;
    }
    pcap_if_t *devices = NULL;
    if (pcap_findalldevs(&devices, errbuf) != 0) {
        fprintf(stderr, "Npcap device enumeration failed: %s\n", errbuf);
        return -1;
    }
    for (pcap_if_t *it = devices; it; it = it->next)
        printf("%s\t%s\n", it->name, it->description ? it->description : "");
    pcap_freealldevs(devices);
    return 0;
}

void log_offload_state(const char *ifname) {
    log_msg("agent", "warn",
            "offload_policy=observe platform=windows adapter=%s; Npcap does not expose GRO/GSO/TSO state",
            ifname);
}

static int apply_transport_filter(pcap_t *pcap, struct self_filter *filter,
                                  uint64_t *applied_generation) {
    char expression[512];
    uint64_t generation = 0;
    int rc = self_filter_expression(filter, expression, sizeof(expression),
                                    &generation);
    if (rc <= 0 || generation == *applied_generation) return rc < 0 ? -1 : 0;
    struct bpf_program program;
    if (pcap_compile(pcap, &program, expression, 1, PCAP_NETMASK_UNKNOWN) != 0) {
        log_msg("agent", "warn", "Npcap BPF compile failed: %s", pcap_geterr(pcap));
        return -1;
    }
    rc = pcap_setfilter(pcap, &program);
    pcap_freecode(&program);
    if (rc != 0) {
        log_msg("agent", "warn", "Npcap BPF install failed: %s", pcap_geterr(pcap));
        return -1;
    }
    *applied_generation = generation;
    log_msg("agent", "info", "Npcap self-transport BPF filter updated");
    return 1;
}

static uint8_t packet_direction(const uint8_t *frame, size_t len,
                                const uint8_t local_mac[6]) {
    if (len < ETH_HLEN) return 0;
    if (!memcmp(frame + 6, local_mac, 6)) return 2;
    if (!memcmp(frame, local_mac, 6)) return 1;
    return 0;
}

static void enqueue_frame(struct capture_context *ctx, const uint8_t *frame,
                          uint32_t frame_len, uint32_t original_len,
                          uint64_t timestamp_ns, uint8_t direction,
                          uint32_t interface_mtu, uint64_t *sequence) {
    struct packet_item item = {0};
    item.data = malloc(frame_len);
    if (!item.data) {
        atomic_fetch_add(&ctx->stats->capture_alloc_failures, 1);
        return;
    }
    memcpy(item.data, frame, frame_len);
    item.len = frame_len;
    item.original_len = original_len;
    item.timestamp_ns = timestamp_ns;
    item.direction = direction;
    if (direction == 2)
        item.flags |= MIRROR_FLAG_TX_DIRECTION | MIRROR_FLAG_CHECKSUM_NOT_READY;
    else if (direction == 1)
        item.flags |= MIRROR_FLAG_RX_DIRECTION;
    if (frame_len > interface_mtu + ETH_HLEN + MIRROR_MAX_VLAN_TAGS * 4u)
        item.flags |= MIRROR_FLAG_GSO_OR_GRO_SUSPECTED;

    if (!strcmp(ctx->cfg->segmentation_mode, "agent") &&
        (item.flags & MIRROR_FLAG_GSO_OR_GRO_SUSPECTED)) {
        struct segmented_batch segments = {0};
        enum segment_result result = segment_tcp_frame(item.data, item.len,
                                                       interface_mtu, &segments);
        if (result == SEGMENT_OK) {
            atomic_fetch_add(&ctx->stats->capture_segmented_aggregates, 1);
            packet_item_free(&item);
            for (size_t i = 0; i < segments.count; i++) {
                struct packet_item segment = {0};
                segment.data = segments.frames[i].data;
                segment.len = segments.frames[i].len;
                segment.original_len = segments.frames[i].len;
                segment.timestamp_ns = timestamp_ns;
                segment.direction = direction;
                segment.flags = MIRROR_FLAG_SOFTWARE_SEGMENTED |
                    (direction == 2 ? MIRROR_FLAG_TX_DIRECTION :
                     direction == 1 ? MIRROR_FLAG_RX_DIRECTION : 0);
                segments.frames[i].data = NULL;
                enqueue_owned_packet(ctx, &segment, sequence);
                atomic_fetch_add(&ctx->stats->capture_generated_segments, 1);
            }
            segmented_batch_free(&segments);
            return;
        }
        if (result == SEGMENT_NO_MEMORY)
            atomic_fetch_add(&ctx->stats->capture_alloc_failures, 1);
        atomic_fetch_add(&ctx->stats->capture_segmentation_failures, 1);
        packet_item_free(&item);
        return;
    }
    if (item.flags & MIRROR_FLAG_GSO_OR_GRO_SUSPECTED)
        atomic_fetch_add(&ctx->stats->capture_forwarded_aggregates, 1);
    enqueue_owned_packet(ctx, &item, sequence);
}

void *capture_thread_main(void *opaque) {
    struct capture_context *ctx = opaque;
    char device[512], description[512], errbuf[PCAP_ERRBUF_SIZE];
    if (resolve_device(ctx->cfg->capture_iface, device, sizeof(device),
                       description, sizeof(description)) != 0) {
        atomic_store(&ctx->failed, 1);
        return NULL;
    }
    int interface_mtu = get_iface_mtu(device);
    uint8_t local_mac[6] = {0};
    if (interface_mtu <= 0 || get_iface_mac(device, local_mac) != 0) {
        log_msg("agent", "error", "could not resolve Windows adapter metadata for %s",
                device);
        atomic_store(&ctx->failed, 1);
        return NULL;
    }
    pcap_t *pcap = pcap_create(device, errbuf);
    if (!pcap) {
        log_msg("agent", "error", "pcap_create failed: %s", errbuf);
        atomic_store(&ctx->failed, 1);
        return NULL;
    }
    uint64_t buffer_size = (uint64_t)ctx->cfg->ring_blocks * ctx->cfg->block_size;
    int pcap_buffer = buffer_size > INT_MAX ? INT_MAX : (int)buffer_size;
    int timestamp_nano = pcap_set_tstamp_precision(pcap, PCAP_TSTAMP_PRECISION_NANO) == 0;
    if (pcap_set_snaplen(pcap, (int)ctx->cfg->max_capture_frame_size) != 0 ||
        pcap_set_promisc(pcap, 1) != 0 ||
        pcap_set_timeout(pcap, (int)ctx->cfg->block_timeout_ms) != 0 ||
        pcap_set_buffer_size(pcap, pcap_buffer) != 0) {
        log_msg("agent", "error", "could not configure Npcap handle: %s", pcap_geterr(pcap));
        pcap_close(pcap);
        atomic_store(&ctx->failed, 1);
        return NULL;
    }
    int activation = pcap_activate(pcap);
    if (activation < 0 || pcap_datalink(pcap) != DLT_EN10MB) {
        log_msg("agent", "error", "Npcap activation/link type failed rc=%d error=%s",
                activation, pcap_geterr(pcap));
        pcap_close(pcap);
        atomic_store(&ctx->failed, 1);
        return NULL;
    }
    log_msg("agent", "info",
            "capture started backend=npcap device=%s description=%s mtu=%d buffer=%d snaplen=%u segmentation_mode=%s",
            device, description, interface_mtu, pcap_buffer,
            ctx->cfg->max_capture_frame_size, ctx->cfg->segmentation_mode);

    uint64_t sequence = 0, filter_generation = 0;
    uint32_t previous_drop = 0;
    unsigned stats_tick = 0;
    while (!atomic_load(&ctx->stop)) {
        apply_transport_filter(pcap, ctx->self_filter, &filter_generation);
        struct pcap_pkthdr *header = NULL;
        const u_char *data = NULL;
        int rc = pcap_next_ex(pcap, &header, &data);
        if (rc == 0) continue;
        if (rc < 0) {
            log_msg("agent", "error", "Npcap read failed: %s", pcap_geterr(pcap));
            atomic_store(&ctx->failed, 1);
            break;
        }
        if (header->caplen < header->len) {
            atomic_fetch_add(&ctx->stats->capture_truncated_packets, 1);
            atomic_fetch_add(&ctx->stats->capture_kernel_truncated_packets, 1);
            continue;
        }
        if (header->caplen > ctx->cfg->max_capture_frame_size) {
            atomic_fetch_add(&ctx->stats->capture_oversized_dropped_packets, 1);
            continue;
        }
        if (self_filter_match(ctx->self_filter, data, header->caplen)) {
            atomic_fetch_add(&ctx->stats->self_transport_packets_filtered, 1);
            atomic_fetch_add(&ctx->stats->self_transport_bytes_filtered, header->caplen);
            continue;
        }
        uint64_t fraction = (uint64_t)header->ts.tv_usec *
                            (timestamp_nano ? 1ULL : 1000ULL);
        uint64_t timestamp_ns = (uint64_t)header->ts.tv_sec * 1000000000ULL + fraction;
        uint8_t direction = packet_direction(data, header->caplen, local_mac);
        enqueue_frame(ctx, data, header->caplen, header->len, timestamp_ns,
                      direction, (uint32_t)interface_mtu, &sequence);

        if (++stats_tick >= 4096) {
            struct pcap_stat stats;
            if (pcap_stats(pcap, &stats) == 0) {
                uint32_t current = stats.ps_drop;
                atomic_fetch_add(&ctx->stats->kernel_ring_dropped_packets,
                                 (uint32_t)(current - previous_drop));
                previous_drop = current;
            }
            stats_tick = 0;
        }
    }
    pcap_close(pcap);
    return NULL;
}
