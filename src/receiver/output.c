#include <receiver/output.h>
#include <mirror/util.h>

#include <errno.h>
#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define OUTPUT_SENDMMSG_MAX 256u

int output_open(struct output_sink *sink, const char *ifname, uint32_t required_mtu,
                uint32_t max_frame_size, char *err, size_t err_len) {
    memset(sink, 0, sizeof(*sink));
    snprintf(sink->ifname, sizeof(sink->ifname), "%s", ifname);
    sink->max_frame_size = max_frame_size;
    sink->ifindex = if_nametoindex(ifname);
    if (!sink->ifindex) {
        snprintf(err, err_len, "output interface %s not found", ifname);
        return -1;
    }
    int mtu = get_iface_mtu(ifname);
    if (mtu < 0 || (uint32_t)mtu < required_mtu) {
        snprintf(err, err_len, "output interface %s MTU %d < required %u", ifname, mtu, required_mtu);
        return -1;
    }
    sink->fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sink->fd < 0) {
        snprintf(err, err_len, "AF_PACKET output socket: %s", strerror(errno));
        return -1;
    }
    return 0;
}

void output_close(struct output_sink *sink) {
    if (sink->fd >= 0) close(sink->fd);
    sink->fd = -1;
}

int output_send_frame(struct output_sink *sink, const uint8_t *frame, size_t frame_len) {
    if (frame_len < ETH_HLEN || frame_len > sink->max_frame_size) return OUTPUT_OVERSIZED;
    struct sockaddr_ll addr = {0};
    addr.sll_family = AF_PACKET;
    addr.sll_protocol = htons(ETH_P_ALL);
    addr.sll_ifindex = sink->ifindex;
    addr.sll_halen = ETH_ALEN;
    memcpy(addr.sll_addr, frame, ETH_ALEN);
    ssize_t n = sendto(sink->fd, frame, frame_len, 0, (struct sockaddr *)&addr, sizeof(addr));
    if (n < 0) return (errno == ENETDOWN || errno == ENETUNREACH || errno == ENXIO) ? OUTPUT_INTERFACE_DOWN : OUTPUT_ERROR;
    return n == (ssize_t)frame_len ? OUTPUT_OK : OUTPUT_SHORT_WRITE;
}

int output_send_frames(struct output_sink *sink, const uint8_t *const *frames,
                       const uint32_t *frame_lengths, int *results,
                       size_t frame_count, uint64_t *send_syscalls) {
    if (!sink || !frames || !frame_lengths || !results ||
        frame_count == 0 || frame_count > OUTPUT_SENDMMSG_MAX)
        return -1;
    struct mmsghdr messages[OUTPUT_SENDMMSG_MAX];
    struct iovec vectors[OUTPUT_SENDMMSG_MAX];
    struct sockaddr_ll addresses[OUTPUT_SENDMMSG_MAX];
    memset(messages, 0, sizeof(messages));
    memset(vectors, 0, sizeof(vectors));
    memset(addresses, 0, sizeof(addresses));
    size_t valid_count = 0;
    size_t valid_index[OUTPUT_SENDMMSG_MAX];
    for (size_t i = 0; i < frame_count; i++) {
        results[i] = OUTPUT_OK;
        if (frame_lengths[i] < ETH_HLEN || frame_lengths[i] > sink->max_frame_size) {
            results[i] = OUTPUT_OVERSIZED;
            continue;
        }
        size_t j = valid_count++;
        valid_index[j] = i;
        addresses[j].sll_family = AF_PACKET;
        addresses[j].sll_protocol = htons(ETH_P_ALL);
        addresses[j].sll_ifindex = sink->ifindex;
        addresses[j].sll_halen = ETH_ALEN;
        memcpy(addresses[j].sll_addr, frames[i], ETH_ALEN);
        vectors[j].iov_base = (void *)frames[i];
        vectors[j].iov_len = frame_lengths[i];
        messages[j].msg_hdr.msg_name = &addresses[j];
        messages[j].msg_hdr.msg_namelen = sizeof(addresses[j]);
        messages[j].msg_hdr.msg_iov = &vectors[j];
        messages[j].msg_hdr.msg_iovlen = 1;
    }
    size_t sent = 0;
    while (sent < valid_count) {
        int rc = sendmmsg(sink->fd, messages + sent,
                          (unsigned)(valid_count - sent), 0);
        if (send_syscalls) (*send_syscalls)++;
        if (rc > 0) {
            for (int j = 0; j < rc; j++) {
                size_t original = valid_index[sent + (size_t)j];
                results[original] = messages[sent + (size_t)j].msg_len ==
                                    frame_lengths[original] ?
                                    OUTPUT_OK : OUTPUT_SHORT_WRITE;
            }
            sent += (size_t)rc;
            continue;
        }
        if (rc < 0 && errno == EINTR) continue;
        int error_result = (errno == ENETDOWN || errno == ENETUNREACH || errno == ENXIO) ?
                           OUTPUT_INTERFACE_DOWN : OUTPUT_ERROR;
        for (; sent < valid_count; sent++)
            results[valid_index[sent]] = error_result;
    }
    return 0;
}
