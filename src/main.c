#include "icmp.h"
#include "types.h"

#include <arpa/inet.h>
#include <bits/types/struct_iovec.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define IPSIZE 16
#define PKTSIZE 64

static const int ttl = 64;
static const struct timeval timeout = { .tv_sec = 2 };
static const char* progname = NULL;

static volatile sig_atomic_t pingloop = 1;

static void
int_handler(int signal) {
    (void)signal;
    pingloop = 0;
}

typedef struct {
    IcmpEchoHeader header;
    u8 msg[PKTSIZE - sizeof(IcmpEchoHeader)];
} Packet;

typedef struct {
    i32 fd;
    const char* dst;
    u8 ip[IPSIZE];
    u8 host[NI_MAXHOST];
    struct sockaddr_in addr;
} PingData;

static void
print_error(const char* restrict fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vdprintf(STDERR_FILENO, fmt, args);
    va_end(args);
}

static void
usage(void) {
    print_error("usage: %s [options] <destination>\n", progname);
}

static bool
is_ipv4(const char* str) {
    u32 dots = 0;
    for (u32 i = 0; str[i]; i++) {
        if (!isdigit(str[i]) && str[i] != '.') {
            return false;
        }
        if (str[i] == '.') {
            dots += 1;
        }
    }

    if (dots > 3) {
        return false;
    }

    return true;
}

static struct sockaddr_in
lookup_addr(const char* dst) {
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_RAW,
        .ai_protocol = IPPROTO_ICMP,
    };
    struct addrinfo* result = NULL;

    const i32 res = getaddrinfo(dst, NULL, &hints, &result);
    if (res != 0) {
        const char* err = gai_strerror(res);
        print_error("%s: %s: %s\n", progname, dst, err);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in out = *(struct sockaddr_in*)result->ai_addr;
    freeaddrinfo(result);

    return out;
}

static void
lookup_hostname(PingData* ping) {
    const size_t addrlen = sizeof(struct sockaddr_in);
    const int res = getnameinfo(
        (struct sockaddr*)&ping->addr,
        addrlen,
        (char*)ping->host,
        sizeof(ping->host),
        NULL,
        0,
        NI_NAMEREQD
    );
    if (res != 0) {
        if (res == EAI_NONAME) return;
        const char* err = gai_strerror(res);
        print_error("%s: %s: %s\n", progname, ping->dst, err);
        exit(EXIT_FAILURE);
    }
}

static u16
checksum(const void* data, u64 len) {
    u32 sum = 0;

    const u16* ptr;
    for (ptr = data; len > 1; len -= 2) {
        sum += *ptr;
    }

    if (len == 1) {
        sum += *(const u8*)ptr;
    }

    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);

    return ~sum;
}

static void
send_ping(PingData* ping) {

    const pid_t pid = getpid();

    if (setsockopt(ping->fd, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) != 0) {
        const char* err = strerror(errno);
        print_error("%s: %s\n", progname, err);
        exit(EXIT_FAILURE);
    }

    setsockopt(ping->fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    Packet pkt = {
            .header = {
                .type = Icmp_EchoRequest,
                .code = 0,
                .id = pid,
            },
        };
    for (u32 i = 0; i < sizeof(pkt.msg); i++) {
        pkt.msg[i] = i + '0';
    }

    u16 msg_count = 0;
    while (pingloop) {
        pkt.header.seq = htons(msg_count++);
        pkt.header.cksum = checksum(&pkt, sizeof(pkt));

        const i64 res = sendto(
            ping->fd,
            &pkt,
            sizeof(pkt),
            0,
            (struct sockaddr*)&ping->addr,
            sizeof(struct sockaddr)
        );

        if (res < 0) {
            const char* err = strerror(errno);
            print_error("%s: %s\n", progname, err);
            exit(EXIT_FAILURE);
        }

        usleep(1000 * 10);

        struct iovec iov;
        struct msghdr rmsg = {
            .msg_name = &ping->addr,
            .msg_namelen = sizeof(ping->addr),
            .msg_iov = &iov,
            .msg_iovlen = 1,
        };
        const i64 bytes = recvmsg(ping->fd, &rmsg, 0);

        if (bytes < 0) {
            const char* err = strerror(errno);
            print_error("%s: %s\n", progname, err);
            exit(EXIT_FAILURE);
        }

        printf("received packet\n");
    }
}

int
main(int argc, char* const* argv) {
    progname = argc > 0 ? argv[0] : "ft_ping";
    if (argc < 2) {
        usage();
        exit(EXIT_FAILURE);
    }

    const bool is_root = getuid() == 0;

    PingData ping = {
        .dst = argv[argc - 1],
    };

    ping.addr = lookup_addr(ping.dst);
    inet_ntop(AF_INET, &ping.addr.sin_addr.s_addr, (char*)ping.ip, INET_ADDRSTRLEN);
    if (!is_ipv4(ping.dst)) {
        lookup_hostname(&ping);
    }

    if (is_root) {
        ping.fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    } else {
        ping.fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    }

    if (ping.fd < 0) {
        if (errno == EPERM || errno == EACCES) {
            print_error("%s: lacking priviledge for icmp socket\n", progname);
        } else {
            const char* err = strerror(errno);
            print_error("%s: %s\n", progname, err);
        }
        exit(EXIT_FAILURE);
    }

    signal(SIGINT, int_handler);

    printf("PING %s (%s)\n", ping.dst, ping.ip);

    send_ping(&ping);

    close(ping.fd);
}
