#include <mirror/test_traffic.h>

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
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
static uint64_t mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec; }
static int u64(const char*s,uint64_t*v){char*e=NULL;errno=0;unsigned long long n=strtoull(s,&e,0);if(errno||!e||*e)return-1;*v=n;return 0;}
static uint16_t u16be(const uint8_t*p){uint16_t n;memcpy(&n,p,2);return ntohs(n);}
static void usage(const char*p){fprintf(stderr,"Usage: %s --interface nad-mirror --run-id N --expected N [--port 55001] [--timeout 120] [--idle-timeout 2]\n",p);}

static const uint8_t *udp_test_payload(const uint8_t *frame,size_t frame_len,uint16_t port,size_t *payload_len){
    if (frame_len < 14) return NULL;
    size_t offset = 14;
    uint16_t ether_type = u16be(frame + 12);
    for (unsigned tags = 0; tags < 4 &&
         (ether_type == 0x8100 || ether_type == 0x88a8 || ether_type == 0x9100); tags++) {
        if (offset + 4 > frame_len) return NULL;
        ether_type = u16be(frame + offset + 2);
        offset += 4;
    }
    if (ether_type != 0x0800 || offset + 20 > frame_len) return NULL;
    const uint8_t *ip = frame + offset;
    size_t ip_header_len = (size_t)(ip[0] & 15) * 4;
    if ((ip[0] >> 4) != 4 || ip_header_len < 20 || offset + ip_header_len > frame_len ||
        ip[9] != 17 || (u16be(ip + 6) & 0x3fff)) return NULL;
    uint16_t ip_total_len = u16be(ip + 2);
    if (ip_total_len < ip_header_len + 8 || offset + ip_total_len > frame_len) return NULL;
    const uint8_t *udp = ip + ip_header_len;
    uint16_t udp_len = u16be(udp + 4);
    if (u16be(udp + 2) != port || udp_len < 8 || udp_len > ip_total_len - ip_header_len)
        return NULL;
    *payload_len = udp_len - 8;
    return udp + 8;
}

int main(int argc,char**argv){
    const char*interface=NULL;uint64_t run_id=0,expected=0,port=55001,timeout=120,idle=2;
    static const struct option opts[]={{"interface",1,NULL,'I'},{"run-id",1,NULL,'i'},{"expected",1,NULL,'n'},
        {"port",1,NULL,'p'},{"timeout",1,NULL,'t'},{"idle-timeout",1,NULL,'w'},{"help",0,NULL,'h'},{0}};
    int o;while((o=getopt_long(argc,argv,"I:i:n:p:t:w:h",opts,NULL))!=-1){
        switch (o) {
            case 'I': interface = optarg; break;
            case 'i': if (u64(optarg, &run_id)) return 2; break;
            case 'n': if (u64(optarg, &expected)) return 2; break;
            case 'p': if (u64(optarg, &port)) return 2; break;
            case 't': if (u64(optarg, &timeout)) return 2; break;
            case 'w': if (u64(optarg, &idle)) return 2; break;
            case 'h': usage(argv[0]); return 0;
            default: usage(argv[0]); return 2;
        }
    }
    if(!interface||!run_id||!expected||!port||port>65535||!timeout||!idle){usage(argv[0]);return 2;}
    unsigned ifindex=if_nametoindex(interface);if(!ifindex){perror("interface");return 1;}
    struct mirror_test_tracker tracker;if(mirror_test_tracker_init(&tracker,expected)){fprintf(stderr,"tracker allocation failed\n");return 1;}
    int fd=socket(AF_PACKET,SOCK_RAW,htons(ETH_P_ALL));if(fd<0){perror("AF_PACKET socket");return 1;}
    int rcvbuf=32*1024*1024;setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&rcvbuf,sizeof(rcvbuf));
    struct sockaddr_ll address={.sll_family=AF_PACKET,.sll_protocol=htons(ETH_P_ALL),.sll_ifindex=(int)ifindex};
    if(bind(fd,(struct sockaddr*)&address,sizeof(address))){perror("AF_PACKET bind");return 1;}
    signal(SIGINT,stop_handler);signal(SIGTERM,stop_handler);uint64_t start=mono_ns(),last=0;uint8_t frame[65536];struct pollfd pollfd={.fd=fd,.events=POLLIN};
    while(!stop_requested){uint64_t now=mono_ns();if(now-start>=timeout*1000000000ULL)break;
        if(last&&tracker.unique_packets>=expected&&now-last>=idle*1000000000ULL)break;
        int ready=poll(&pollfd,1,200);if(ready<0&&errno==EINTR)continue;if(ready<0){perror("poll");break;}if(!ready)continue;
        ssize_t n=recv(fd,frame,sizeof(frame),0);if(n<0){if(errno==EINTR)continue;perror("recv");break;}size_t payload_len=0;
        const uint8_t*payload=udp_test_payload(frame,(size_t)n,(uint16_t)port,&payload_len);if(!payload)continue;
        uint64_t before=tracker.matching_packets;mirror_test_tracker_record(&tracker,payload,payload_len,run_id);if(tracker.matching_packets!=before)last=mono_ns();}
    mirror_test_tracker_print("verifier",&tracker,run_id);bool success=mirror_test_tracker_success(&tracker);
    mirror_test_tracker_destroy(&tracker);close(fd);return success?0:1;
}
