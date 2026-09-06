#ifndef AGENT_SEGMENT_H
#define AGENT_SEGMENT_H

#include <stddef.h>
#include <stdint.h>

struct segmented_frame {
    uint8_t *data;
    uint32_t len;
};

struct segmented_batch {
    struct segmented_frame *frames;
    size_t count;
};

enum segment_result {
    SEGMENT_NOT_NEEDED = 0,
    SEGMENT_OK = 1,
    SEGMENT_UNSUPPORTED = -1,
    SEGMENT_NO_MEMORY = -2
};

enum segment_result segment_tcp_frame(const uint8_t *frame, size_t frame_len,
                                      uint32_t interface_mtu,
                                      struct segmented_batch *out);
void segmented_batch_free(struct segmented_batch *batch);

#endif
