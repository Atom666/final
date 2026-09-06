#include <agent/platform.h>
#include <mirror/config.h>
#include <mirror/util.h>

#include <stdio.h>
#include <string.h>

static _Atomic int *platform_stop;

#ifdef _WIN32
#include <bcrypt.h>
#include <psapi.h>
#include <windows.h>

static BOOL WINAPI console_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT ||
        event == CTRL_CLOSE_EVENT || event == CTRL_SHUTDOWN_EVENT) {
        if (platform_stop) atomic_store(platform_stop, 1);
        return TRUE;
    }
    return FALSE;
}

int agent_platform_install_stop_handler(_Atomic int *stop) {
    platform_stop = stop;
    return SetConsoleCtrlHandler(console_handler, TRUE) ? 0 : -1;
}

static uint64_t filetime_us(FILETIME value) {
    ULARGE_INTEGER integer;
    integer.LowPart = value.dwLowDateTime;
    integer.HighPart = value.dwHighDateTime;
    return integer.QuadPart / 10ULL;
}

uint64_t agent_process_cpu_us(void) {
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        return 0;
    return filetime_us(kernel) + filetime_us(user);
}

uint64_t agent_current_rss_bytes(void) {
    PROCESS_MEMORY_COUNTERS counters;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return 0;
    return (uint64_t)counters.WorkingSetSize;
}

uint32_t agent_process_id(void) { return GetCurrentProcessId(); }

void agent_sleep_interruptible(uint32_t milliseconds, const _Atomic int *stop) {
    while (milliseconds && !atomic_load(stop)) {
        DWORD chunk = milliseconds > 100 ? 100 : milliseconds;
        Sleep(chunk);
        milliseconds -= chunk;
    }
}

int agent_get_hostname(char *out, size_t out_len) {
    DWORD length = (DWORD)out_len;
    return GetComputerNameExA(ComputerNameDnsHostname, out, &length) ? 0 : -1;
}

int agent_get_os_release(char *out, size_t out_len) {
    OSVERSIONINFOEXA version = {0};
    version.dwOSVersionInfoSize = sizeof(version);
#pragma warning(push)
#pragma warning(disable:4996)
    if (!GetVersionExA((OSVERSIONINFOA *)&version)) return -1;
#pragma warning(pop)
    snprintf(out, out_len, "Windows %lu.%lu build %lu",
             version.dwMajorVersion, version.dwMinorVersion,
             version.dwBuildNumber);
    return 0;
}

int agent_generate_uuid(uint8_t uuid[16]) {
    if (BCryptGenRandom(NULL, uuid, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return -1;
    uuid[6] = (uint8_t)((uuid[6] & 0x0f) | 0x40);
    uuid[8] = (uint8_t)((uuid[8] & 0x3f) | 0x80);
    return 0;
}

int agent_set_socket_send_timeout(mirror_socket_t socket, uint32_t seconds) {
    DWORD timeout_ms = seconds > UINT32_MAX / 1000 ? UINT32_MAX : seconds * 1000;
    return setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
                      (const char *)&timeout_ms, sizeof(timeout_ms));
}

#else
#include <errno.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

static void signal_handler(int signal_number) {
    (void)signal_number;
    if (platform_stop) atomic_store(platform_stop, 1);
}

int agent_platform_install_stop_handler(_Atomic int *stop) {
    platform_stop = stop;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    return sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)
        ? -1 : 0;
}

uint64_t agent_process_cpu_us(void) {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
    return (uint64_t)usage.ru_utime.tv_sec * 1000000ULL + usage.ru_utime.tv_usec +
           (uint64_t)usage.ru_stime.tv_sec * 1000000ULL + usage.ru_stime.tv_usec;
}

uint64_t agent_current_rss_bytes(void) {
    FILE *file = fopen("/proc/self/statm", "r");
    unsigned long total_pages = 0, resident_pages = 0;
    if (!file) return 0;
    int parsed = fscanf(file, "%lu %lu", &total_pages, &resident_pages);
    fclose(file);
    (void)total_pages;
    long page_size = sysconf(_SC_PAGESIZE);
    return parsed == 2 && page_size > 0 ?
        (uint64_t)resident_pages * (uint64_t)page_size : 0;
}

uint32_t agent_process_id(void) { return (uint32_t)getpid(); }

void agent_sleep_interruptible(uint32_t milliseconds, const _Atomic int *stop) {
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (long)(milliseconds % 1000) * 1000000L
    };
    while (!atomic_load(stop) && nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
}

int agent_get_hostname(char *out, size_t out_len) {
    return gethostname(out, out_len);
}

int agent_get_os_release(char *out, size_t out_len) {
    struct utsname value;
    if (uname(&value) != 0) return -1;
    snprintf(out, out_len, "%s", value.release);
    return 0;
}

int agent_generate_uuid(uint8_t uuid[16]) {
    FILE *file = fopen("/proc/sys/kernel/random/uuid", "r");
    char text[64];
    if (!file) return -1;
    int result = fgets(text, sizeof(text), file) ? parse_uuid(text, uuid) : -1;
    fclose(file);
    return result;
}

int agent_set_socket_send_timeout(mirror_socket_t socket, uint32_t seconds) {
    struct timeval timeout = { .tv_sec = (time_t)seconds, .tv_usec = 0 };
    return setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}
#endif

uint32_t agent_random_next(uint32_t *state) {
    uint32_t value = *state ? *state : 0x9e3779b9u;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

int agent_peer_ipv4(mirror_socket_t socket, char *out, size_t out_len) {
    struct sockaddr_in address;
    mirror_socklen_t length = (mirror_socklen_t)sizeof(address);
    out[0] = 0;
    if (getpeername(socket, (struct sockaddr *)&address, &length) != 0 ||
        address.sin_family != AF_INET) return -1;
    return inet_ntop(AF_INET, &address.sin_addr, out, out_len) ? 0 : -1;
}
