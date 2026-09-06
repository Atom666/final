#include <mirror/test_traffic.h>

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested;
static void stop_handler(int signal_number) { (void)signal_number; stop_requested = 1; }
static uint64_t mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec; }
static int u64(const char *s, uint64_t *v) { char *e = NULL; errno = 0; unsigned long long n = strtoull(s, &e, 0); if (errno || !e || *e) return -1; *v = n; return 0; }
static void usage(const char *p) { fprintf(stderr, "Usage: %s --run-id N --expected N [--bind 0.0.0.0] [--port 55001] [--timeout 120] [--idle-timeout 2]\n", p); }

int main(int argc, char **argv) {
    const char *bind_address = "0.0.0.0"; uint64_t run_id = 0, expected = 0, port = 55001, timeout = 120, idle = 2;
    static const struct option opts[] = {{"run-id",1,NULL,'i'},{"expected",1,NULL,'n'},{"bind",1,NULL,'b'},
        {"port",1,NULL,'p'},{"timeout",1,NULL,'t'},{"idle-timeout",1,NULL,'w'},{"help",0,NULL,'h'},{0}};
    int o; while ((o=getopt_long(argc,argv,"i:n:b:p:t:w:h",opts,NULL))!=-1) {
        switch (o) {
            case 'i': if (u64(optarg, &run_id)) return 2; break;
            case 'n': if (u64(optarg, &expected)) return 2; break;
            case 'b': bind_address = optarg; break;
            case 'p': if (u64(optarg, &port)) return 2; break;
            case 't': if (u64(optarg, &timeout)) return 2; break;
            case 'w': if (u64(optarg, &idle)) return 2; break;
            case 'h': usage(argv[0]); return 0;
            default: usage(argv[0]); return 2;
        }
    }
    if(!run_id||!expected||!port||port>65535||!timeout||!idle){usage(argv[0]);return 2;}
    struct mirror_test_tracker tracker; if(mirror_test_tracker_init(&tracker,expected)){fprintf(stderr,"tracker allocation failed\n");return 1;}
    int fd=socket(AF_INET,SOCK_DGRAM,0); if(fd<0){perror("socket");return 1;}
    int rcvbuf=32*1024*1024; setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&rcvbuf,sizeof(rcvbuf));
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons((uint16_t)port)};
    if(inet_pton(AF_INET,bind_address,&address.sin_addr)!=1||bind(fd,(struct sockaddr*)&address,sizeof(address))){perror("bind");return 1;}
    signal(SIGINT,stop_handler); signal(SIGTERM,stop_handler);
    uint64_t start=mono_ns(),last=0; uint8_t payload[65536]; struct pollfd pollfd={.fd=fd,.events=POLLIN};
    while(!stop_requested) {
        uint64_t now=mono_ns();
        if(now-start>=timeout*1000000000ULL)break;
        if(last&&tracker.unique_packets>=expected&&now-last>=idle*1000000000ULL)break;
        int ready=poll(&pollfd,1,200); if(ready<0&&errno==EINTR)continue; if(ready<0){perror("poll");break;} if(!ready)continue;
        ssize_t n=recv(fd,payload,sizeof(payload),0); if(n<0){if(errno==EINTR)continue;perror("recv");break;}
        uint64_t before=tracker.matching_packets; mirror_test_tracker_record(&tracker,payload,(size_t)n,run_id);
        if(tracker.matching_packets!=before)last=mono_ns();
    }
    mirror_test_tracker_print("sink",&tracker,run_id); bool success=mirror_test_tracker_success(&tracker);
    mirror_test_tracker_destroy(&tracker); close(fd); return success?0:1;
}
