#include <mirror/test_traffic.h>

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static void usage(const char *program) {
    fprintf(stderr, "Usage: %s --destination IPv4 --run-id N --count N "
            "[--port 55001] [--source-port 55000] [--size 1200] [--rate-mbps 10]\n", program);
}

static int parse_u64(const char *text, uint64_t *value) {
    char *end = NULL; errno = 0;
    unsigned long long parsed = strtoull(text, &end, 0);
    if (errno || !end || *end) return -1;
    *value = (uint64_t)parsed; return 0;
}

static uint64_t monotonic_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void sleep_until(uint64_t target_ns) {
    struct timespec target = { .tv_sec = (time_t)(target_ns / 1000000000ULL),
                               .tv_nsec = (long)(target_ns % 1000000000ULL) };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &target, NULL) == EINTR) {}
}

int main(int argc, char **argv) {
    const char *destination = NULL;
    uint64_t run_id = 0, count = 0;
    uint64_t size64 = 1200, port64 = 55001, source_port64 = 55000;
    double rate_mbps = 10.0;
    static const struct option options[] = {
        {"destination", required_argument, NULL, 'd'}, {"run-id", required_argument, NULL, 'i'},
        {"count", required_argument, NULL, 'n'}, {"size", required_argument, NULL, 's'},
        {"port", required_argument, NULL, 'p'}, {"source-port", required_argument, NULL, 'q'},
        {"rate-mbps", required_argument, NULL, 'r'}, {"help", no_argument, NULL, 'h'}, {0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "d:i:n:s:p:q:r:h", options, NULL)) != -1) {
        switch (opt) {
            case 'd': destination = optarg; break;
            case 'i': if (parse_u64(optarg, &run_id)) return 2; break;
            case 'n': if (parse_u64(optarg, &count)) return 2; break;
            case 's': if (parse_u64(optarg, &size64)) return 2; break;
            case 'p': if (parse_u64(optarg, &port64)) return 2; break;
            case 'q': if (parse_u64(optarg, &source_port64)) return 2; break;
            case 'r': {
                char *end = NULL; rate_mbps = strtod(optarg, &end);
                if (!end || *end) return 2;
                break;
            }
            case 'h': usage(argv[0]); return 0;
            default: usage(argv[0]); return 2;
        }
    }
    if (!destination || !run_id || !count || count > MIRROR_TEST_MAX_EXPECTED ||
        size64 < MIRROR_TEST_MIN_PACKET_SIZE ||
        size64 > MIRROR_TEST_MAX_PACKET_SIZE || !port64 || port64 > 65535 ||
        !source_port64 || source_port64 > 65535 || rate_mbps < 0.0 || rate_mbps > 100000.0) {
        usage(argv[0]); return 2;
    }

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *addresses = NULL;
    char port_text[16]; snprintf(port_text, sizeof(port_text), "%" PRIu64, port64);
    if (getaddrinfo(destination, port_text, &hints, &addresses) != 0) {
        fprintf(stderr, "cannot resolve destination %s\n", destination); return 1;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); freeaddrinfo(addresses); return 1; }
    int buffer_size = 16 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size));
    struct sockaddr_in source = { .sin_family = AF_INET, .sin_port = htons((uint16_t)source_port64),
                                  .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(fd, (struct sockaddr *)&source, sizeof(source)) != 0) {
        perror("bind source port"); close(fd); freeaddrinfo(addresses); return 1;
    }

    uint8_t packet[MIRROR_TEST_MAX_PACKET_SIZE];
    uint64_t sent = 0, errors = 0, start = monotonic_ns();
    long double interval_ns = rate_mbps > 0.0 ? ((long double)size64 * 8000.0L / rate_mbps) : 0.0L;
    for (uint64_t sequence = 1; sequence <= count; sequence++) {
        if (interval_ns > 0.0L && sequence > 1)
            sleep_until(start + (uint64_t)((long double)(sequence - 1) * interval_ns));
        if (mirror_test_build_packet(packet, sizeof(packet), (uint32_t)size64, run_id, sequence) != 0) {
            fprintf(stderr, "packet build failed\n"); break;
        }
        ssize_t n = sendto(fd, packet, (size_t)size64, 0, addresses->ai_addr, addresses->ai_addrlen);
        if (n == (ssize_t)size64) sent++; else errors++;
    }
    uint64_t elapsed = monotonic_ns() - start;
    double actual_mbps = elapsed ? (double)sent * (double)size64 * 8000.0 / (double)elapsed : 0.0;
    printf("component=sender run_id=%" PRIu64 " requested=%" PRIu64
           " sent=%" PRIu64 " errors=%" PRIu64 " packet_size=%" PRIu64
           " elapsed_sec=%.3f payload_mbps=%.3f result=%s\n",
           run_id, count, sent, errors, size64, (double)elapsed / 1e9, actual_mbps,
           sent == count && !errors ? "PASS" : "FAIL");
    close(fd); freeaddrinfo(addresses);
    return sent == count && !errors ? 0 : 1;
}
