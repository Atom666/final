#include <assert.h>
#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <receiver/output.h>

static void run_size(struct output_sink *sink, int rx, size_t len, unsigned seed) {
    unsigned char *frame = malloc(len), *got = malloc(len + 64); assert(frame && got);
    for (size_t i = 0; i < len; i++) frame[i] = (unsigned char)(i + seed);
    frame[0] &= 0xfe; frame[12] = 0x88; frame[13] = 0xb5;
    assert(output_send_frame(sink, frame, len) == OUTPUT_OK);
    struct pollfd pfd = { .fd = rx, .events = POLLIN }; assert(poll(&pfd, 1, 2000) == 1);
    ssize_t n = recv(rx, got, len + 64, 0); assert(n == (ssize_t)len); assert(memcmp(frame, got, len) == 0);
    free(frame); free(got);
}

static void run_batch(struct output_sink *sink, int rx) {
    const uint32_t lengths[] = {64, 512, 1514, 1518};
    uint8_t *frames[4] = {0};
    const uint8_t *frame_ptrs[4];
    int results[4] = {0};
    uint64_t syscalls = 0;
    for (size_t i = 0; i < 4; i++) {
        frames[i] = malloc(lengths[i]);
        assert(frames[i]);
        for (size_t j = 0; j < lengths[i]; j++)
            frames[i][j] = (uint8_t)(j + i * 17u);
        frames[i][0] &= 0xfe;
        frames[i][12] = 0x88;
        frames[i][13] = 0xb6;
        frame_ptrs[i] = frames[i];
    }
    assert(output_send_frames(sink, frame_ptrs, lengths, results, 4, &syscalls) == 0);
    assert(syscalls >= 1 && syscalls <= 4);
    for (size_t i = 0; i < 4; i++) {
        assert(results[i] == OUTPUT_OK);
        uint8_t got[1600];
        struct pollfd pfd = {.fd = rx, .events = POLLIN};
        assert(poll(&pfd, 1, 2000) == 1);
        ssize_t n = recv(rx, got, sizeof(got), 0);
        assert(n == (ssize_t)lengths[i]);
        assert(memcmp(got, frames[i], lengths[i]) == 0);
        free(frames[i]);
    }
}

int main(void) {
    int idx = if_nametoindex("nad-mirror");
    if (!idx) { fprintf(stderr, "nad-mirror missing; run scripts/mirror-interface-setup.sh as root\n"); return 77; }
    int rx = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL)); assert(rx >= 0);
    struct sockaddr_ll a = { .sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL), .sll_ifindex = idx };
    assert(bind(rx, (struct sockaddr *)&a, sizeof(a)) == 0);
    struct output_sink sink; char err[256]; assert(output_open(&sink, "mirror-rx", 9216, 9216, err, sizeof(err)) == 0);
    const size_t sizes[] = {64, 512, 1514, 1518, 9000};
    for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) run_size(&sink, rx, sizes[i], (unsigned)i);
    run_batch(&sink, rx);
    unsigned char too_big[9217] = {0}; assert(output_send_frame(&sink, too_big, sizeof(too_big)) == OUTPUT_OVERSIZED);
    output_close(&sink); close(rx); return 0;
}
