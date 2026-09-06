#include <mirror/util.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <iphlpapi.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <net/if.h>
#include <netdb.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

uint64_t now_ns(void) {
#ifdef _WIN32
    FILETIME ft;
    ULARGE_INTEGER value;
    GetSystemTimePreciseAsFileTime(&ft);
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return (value.QuadPart - 116444736000000000ULL) * 100ULL;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

uint64_t monotonic_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    uint64_t seconds = (uint64_t)(counter.QuadPart / frequency.QuadPart);
    uint64_t remainder = (uint64_t)(counter.QuadPart % frequency.QuadPart);
    return seconds * 1000000000ULL +
           remainder * 1000000000ULL / (uint64_t)frequency.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

void log_msg(const char *component, const char *level, const char *fmt, ...) {
    uint64_t timestamp = now_ns();
    fprintf(stderr, "ts=%llu.%09llu component=%s level=%s msg=\"",
            (unsigned long long)(timestamp / 1000000000ULL),
            (unsigned long long)(timestamp % 1000000000ULL), component, level);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\"\n");
}

int set_nonblock(mirror_socket_t fd, bool nonblock) {
#ifdef _WIN32
    u_long mode = nonblock ? 1 : 0;
    return ioctlsocket(fd, FIONBIO, &mode) == 0 ? 0 : -1;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (nonblock) flags |= O_NONBLOCK; else flags &= ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags);
#endif
}

mirror_socket_t connect_tcp(const char *host, uint16_t port, char *local_ip,
                            size_t local_ip_len, uint16_t *local_port) {
    char port_s[16];
    snprintf(port_s, sizeof(port_s), "%u", port);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, port_s, &hints, &res) != 0) return -1;
    mirror_socket_t fd = MIRROR_INVALID_SOCKET;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == MIRROR_INVALID_SOCKET) continue;
        if (connect(fd, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        close_socket(fd);
        fd = MIRROR_INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (fd == MIRROR_INVALID_SOCKET) return MIRROR_INVALID_SOCKET;
    struct sockaddr_in sin;
    mirror_socklen_t sl = (mirror_socklen_t)sizeof(sin);
    if (getsockname(fd, (struct sockaddr *)&sin, &sl) == 0) {
        inet_ntop(AF_INET, &sin.sin_addr, local_ip, local_ip_len);
        if (local_port) *local_port = ntohs(sin.sin_port);
    }
    return fd;
}

mirror_socket_t listen_tcp(const char *addr, uint16_t port, int backlog) {
    mirror_socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == MIRROR_INVALID_SOCKET) return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
    struct sockaddr_in sin = {0};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    if (inet_pton(AF_INET, addr, &sin.sin_addr) != 1) {
        close_socket(fd); return -1;
    }
    if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) != 0) { close_socket(fd); return -1; }
    if (listen(fd, backlog) != 0) { close_socket(fd); return -1; }
    return fd;
}

mirror_ssize_t read_file_trimmed(const char *path, char *buf, size_t len) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(buf, (int)len, f)) { fclose(f); return -1; }
    fclose(f);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ' || buf[n - 1] == '\t')) buf[--n] = 0;
    return (mirror_ssize_t)n;
}

#ifdef _WIN32
static int wide_to_utf8(const wchar_t *wide, char *out, size_t out_len) {
    if (!wide || !out_len) return -1;
    int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)out_len,
                                NULL, NULL);
    return n > 0 ? 0 : -1;
}

static IP_ADAPTER_ADDRESSES *find_adapter(const char *name,
                                           IP_ADAPTER_ADDRESSES **storage) {
    ULONG size = 16384;
    IP_ADAPTER_ADDRESSES *list = NULL;
    for (int attempt = 0; attempt < 2; attempt++) {
        list = malloc(size);
        if (!list) return NULL;
        ULONG rc = GetAdaptersAddresses(AF_UNSPEC,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            NULL, list, &size);
        if (rc == NO_ERROR) break;
        free(list);
        list = NULL;
        if (rc != ERROR_BUFFER_OVERFLOW) return NULL;
    }
    if (!list) return NULL;
    for (IP_ADAPTER_ADDRESSES *it = list; it; it = it->Next) {
        char friendly[512] = "", description[512] = "";
        wide_to_utf8(it->FriendlyName, friendly, sizeof(friendly));
        wide_to_utf8(it->Description, description, sizeof(description));
        if (!strcmp(name, it->AdapterName) || strstr(name, it->AdapterName) ||
            !strcmp(name, friendly) || !strcmp(name, description)) {
            *storage = list;
            return it;
        }
    }
    free(list);
    return NULL;
}

int get_iface_mtu(const char *ifname) {
    IP_ADAPTER_ADDRESSES *storage = NULL;
    IP_ADAPTER_ADDRESSES *adapter = find_adapter(ifname, &storage);
    int result = adapter ? (int)adapter->Mtu : -1;
    free(storage);
    return result;
}

int get_iface_mac(const char *ifname, uint8_t mac[6]) {
    IP_ADAPTER_ADDRESSES *storage = NULL;
    IP_ADAPTER_ADDRESSES *adapter = find_adapter(ifname, &storage);
    int result = -1;
    if (adapter && adapter->PhysicalAddressLength >= 6) {
        memcpy(mac, adapter->PhysicalAddress, 6);
        result = 0;
    }
    free(storage);
    return result;
}

uint32_t get_iface_index(const char *ifname) {
    IP_ADAPTER_ADDRESSES *storage = NULL;
    IP_ADAPTER_ADDRESSES *adapter = find_adapter(ifname, &storage);
    uint32_t result = adapter ? adapter->IfIndex : 0;
    free(storage);
    return result;
}

void close_socket(mirror_socket_t fd) { closesocket(fd); }
int socket_last_error(void) { return WSAGetLastError(); }
const char *socket_error_string(int error, char *buf, size_t len) {
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             NULL, (DWORD)error, 0, buf, (DWORD)len, NULL);
    if (!n) snprintf(buf, len, "Winsock error %d", error);
    while (n && (buf[n - 1] == '\r' || buf[n - 1] == '\n')) buf[--n] = 0;
    return buf;
}
int network_init(void) {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0 ? 0 : -1;
}
void network_cleanup(void) { WSACleanup(); }

#else
int get_iface_mtu(const char *ifname) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    int rc = ioctl(fd, SIOCGIFMTU, &ifr);
    close(fd);
    return rc == 0 ? ifr.ifr_mtu : -1;
}

int get_iface_mac(const char *ifname, uint8_t mac[6]) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    int rc = ioctl(fd, SIOCGIFHWADDR, &ifr);
    close(fd);
    if (rc != 0) return -1;
    memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    return 0;
}

uint32_t get_iface_index(const char *ifname) { return if_nametoindex(ifname); }
void close_socket(mirror_socket_t fd) { close(fd); }
int socket_last_error(void) { return errno; }
const char *socket_error_string(int error, char *buf, size_t len) {
    snprintf(buf, len, "%s", strerror(error));
    return buf;
}
int network_init(void) { return 0; }
void network_cleanup(void) {}
#endif
