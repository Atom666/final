#ifndef MIRROR_UTIL_H
#define MIRROR_UTIL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <mirror/net.h>
#ifdef _WIN32
typedef long mirror_ssize_t;
#else
#include <sys/types.h>
typedef ssize_t mirror_ssize_t;
#endif

#if defined(__GNUC__) || defined(__clang__)
#define MIRROR_PRINTF_FORMAT(a, b) __attribute__((format(printf, a, b)))
#else
#define MIRROR_PRINTF_FORMAT(a, b)
#endif

uint64_t now_ns(void);
uint64_t monotonic_ns(void);
void log_msg(const char *component, const char *level, const char *fmt, ...)
    MIRROR_PRINTF_FORMAT(3, 4);
int set_nonblock(mirror_socket_t fd, bool nonblock);
mirror_socket_t connect_tcp(const char *host, uint16_t port, char *local_ip,
                            size_t local_ip_len, uint16_t *local_port);
mirror_socket_t listen_tcp(const char *addr, uint16_t port, int backlog);
mirror_ssize_t read_file_trimmed(const char *path, char *buf, size_t len);
int get_iface_mtu(const char *ifname);
int get_iface_mac(const char *ifname, uint8_t mac[6]);
uint32_t get_iface_index(const char *ifname);
void close_socket(mirror_socket_t fd);
int socket_last_error(void);
const char *socket_error_string(int error, char *buf, size_t len);
int network_init(void);
void network_cleanup(void);

#endif
