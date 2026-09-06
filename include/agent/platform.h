#ifndef AGENT_PLATFORM_H
#define AGENT_PLATFORM_H

#include <mirror/net.h>

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

int agent_platform_install_stop_handler(_Atomic int *stop);
uint64_t agent_process_cpu_us(void);
uint64_t agent_current_rss_bytes(void);
uint32_t agent_process_id(void);
void agent_sleep_interruptible(uint32_t milliseconds, const _Atomic int *stop);
uint32_t agent_random_next(uint32_t *state);
int agent_get_hostname(char *out, size_t out_len);
int agent_get_os_release(char *out, size_t out_len);
int agent_generate_uuid(uint8_t uuid[16]);
int agent_set_socket_send_timeout(mirror_socket_t socket, uint32_t seconds);
int agent_peer_ipv4(mirror_socket_t socket, char *out, size_t out_len);

#endif
