#ifndef MIRROR_NET_H
#define MIRROR_NET_H

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET mirror_socket_t;
typedef int mirror_socklen_t;
#define MIRROR_INVALID_SOCKET INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
typedef int mirror_socket_t;
typedef socklen_t mirror_socklen_t;
#define MIRROR_INVALID_SOCKET (-1)
#endif

#ifndef ETH_HLEN
#define ETH_HLEN 14
#endif
#ifndef ETH_P_IP
#define ETH_P_IP 0x0800
#endif
#ifndef ETH_P_IPV6
#define ETH_P_IPV6 0x86dd
#endif
#ifndef ETH_P_8021Q
#define ETH_P_8021Q 0x8100
#endif
#ifndef ETH_P_8021AD
#define ETH_P_8021AD 0x88a8
#endif

#endif
