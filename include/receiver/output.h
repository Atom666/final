#ifndef RECEIVER_OUTPUT_H
#define RECEIVER_OUTPUT_H

#include <stddef.h>
#include <stdint.h>

struct output_sink {
    int fd;
    int ifindex;
    char ifname[64];
    uint32_t max_frame_size;
};

enum output_result { OUTPUT_OK = 0, OUTPUT_ERROR = -1, OUTPUT_SHORT_WRITE = -2,
                     OUTPUT_OVERSIZED = -3, OUTPUT_INTERFACE_DOWN = -4 };

int output_open(struct output_sink *sink, const char *ifname, uint32_t required_mtu,
                uint32_t max_frame_size, char *err, size_t err_len);
void output_close(struct output_sink *sink);
int output_send_frame(struct output_sink *sink, const uint8_t *frame, size_t frame_len);
int output_send_frames(struct output_sink *sink, const uint8_t *const *frames,
                       const uint32_t *frame_lengths, int *results,
                       size_t frame_count, uint64_t *send_syscalls);

#endif
