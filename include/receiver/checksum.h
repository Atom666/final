#ifndef RECEIVER_CHECKSUM_H
#define RECEIVER_CHECKSUM_H

#include <stddef.h>
#include <stdint.h>

enum checksum_repair_result {
    CHECKSUM_REPAIR_OK = 0,
    CHECKSUM_REPAIR_UNSUPPORTED = 1,
    CHECKSUM_REPAIR_MALFORMED = 2
};

enum checksum_repair_result checksum_repair_offload_frame(uint8_t *frame,
                                                           size_t frame_len);

#endif
