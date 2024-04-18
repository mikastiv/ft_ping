#include "ping.h"
#include "types.h"
#include "utils.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static i32 TTL = 115;
// static const struct timeval TIMEOUT = { .tv_sec = 2 };

static const char* progname = NULL;
Options options = { 0 };

static volatile sig_atomic_t pingloop = 1;

static void
int_handler(int signal) {
    (void)signal;
    pingloop = 0;
    printf("\n");
}

static void
print_option(const char* name, const char* desc) {
    dprintf(STDERR_FILENO, "  %-20s%s\n", name, desc);
}

static void
usage(void) {
    dprintf(STDERR_FILENO, "usage: %s [options] <destination>\n\n", progname);
    dprintf(STDERR_FILENO, "options: \n");
    print_option("<destination>", "dns name or ip address");
    print_option("-h", "print help ane exit");
    print_option("-v", "verbose output");
    print_option("-n", "no dns name resolution");
    print_option("-t <ttl>", "define time to live");
}

static struct sockaddr_in
lookup_addr(const char* dst) {
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_RAW,
        .ai_protocol = IPPROTO_ICMP,
    };
    struct addrinfo* result = NULL;

    const i32 res = getaddrinfo((const char*)dst, NULL, &hints, &result);
    if (res != 0) {
        const char* err = gai_strerror(res);
        dprintf(STDERR_FILENO, "%s: %s: %s\n", progname, dst, err);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in out = *(struct sockaddr_in*)result->ai_addr;
    freeaddrinfo(result);

    return out;
}

static bool
dns_lookup(struct sockaddr_in addr, char* buffer, const u64 buf_size) {
    const i32 res = getnameinfo(
        (struct sockaddr*)&addr,
        sizeof(struct sockaddr_in),
        buffer,
        buf_size,
        NULL,
        0,
        NI_NAMEREQD
    );

    if (res != 0) {
        buffer[0] = 0;

        if (res == EAI_NONAME || res == EAI_AGAIN) return false;

        char tmp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, (const char*)&addr.sin_addr.s_addr, tmp, sizeof(tmp));

        const char* err = gai_strerror(res);
        dprintf(STDERR_FILENO, "%s: %s: %s\n", progname, tmp, err);
        exit(EXIT_FAILURE);
    }

    return true;
}

static u16
checksum(const void* data, u64 len) {
    u32 sum = 0;

    const u16* ptr;
    for (ptr = data; len > 1; len -= 2) {
        sum += *ptr;
        ptr++;
    }

    if (len == 1) {
        sum += *(const u8*)ptr;
    }

    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);

    return ~sum;
}

static bool
decode_msg(const u8* buffer, const u64 buffer_size, Packet* out, u32* ip_hdr_size) {
    const struct ip* ip_header = (struct ip*)buffer;
    const u32 header_size = ip_header->ip_hl << 2;
    *ip_hdr_size = header_size;

    Packet* pkt = (Packet*)(buffer + header_size);
    *out = *pkt;

    if (pkt->header.type != Icmp_EchoReply) {
        return false;
    }

    if (buffer_size < header_size + PKTSIZE) {
        return false;
    }

    const u16 cksum = pkt->header.cksum;
    pkt->header.cksum = 0;
    pkt->header.cksum = checksum(pkt, buffer_size - header_size);
    if (cksum != pkt->header.cksum) {
        return false;
    }
    return true;
}

static void
init_socket(const i32 fd) {
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));

    if (setsockopt(fd, IPPROTO_IP, IP_TTL, &TTL, sizeof(TTL)) != 0) {
        const char* err = strerror(errno);
        dprintf(STDERR_FILENO, "%s: %s\n", progname, err);
        exit(EXIT_FAILURE);
    }

    // if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &TIMEOUT, sizeof(TIMEOUT)) != 0) {
    //     const char* err = strerror(errno);
    //     dprintf(STDERR_FILENO, "%s: %s\n", progname, err);
    //     exit(EXIT_FAILURE);
    // }
}

static Packet
init_packet(const pid_t pid, const u16 seq) {
    Packet pkt = {
            .header = {
                .type = Icmp_EchoRequest,
                .code = 0,
                .id = pid,
                .seq = htons(seq),
            },
        };
    for (u32 i = 0; i < sizeof(pkt.msg); i++) {
        pkt.msg[i] = i + '0';
    }

    pkt.header.cksum = checksum(&pkt, sizeof(pkt));

    return pkt;
}

static void
send_ping(PingData* ping) {
    const pid_t pid = getpid();

    init_socket(ping->fd);

    printf("PING %s (%s) %lu data bytes\n", ping->dst, ping->ip, sizeof(Packet) - MIN_ICMPSIZE);

    u16 msg_count = 0;
    u16 pkt_transmitted = 0;
    u16 pkt_received = 0;

    while (pingloop) {
        Packet pkt = init_packet(pid, msg_count++);

        struct timeval start;
        gettimeofday(&start, NULL);

        const i64 res = sendto(
            ping->fd,
            &pkt,
            sizeof(pkt),
            0,
            (struct sockaddr*)&ping->addr,
            sizeof(struct sockaddr)
        );

        if (res == 0) {
            dprintf(STDERR_FILENO, "%s: socket closed\n", progname);
            exit(EXIT_FAILURE);
        }

        if (res < 0) {
            const char* err = strerror(errno);
            dprintf(STDERR_FILENO, "%s: %s\n", progname, err);
            exit(EXIT_FAILURE);
        }

        pkt_transmitted++;

        u8 buffer[256];
        struct iovec iov = {
            .iov_base = buffer,
            .iov_len = sizeof(buffer),
        };

        struct sockaddr_in addr = ping->addr;
        struct msghdr rmsg = {
            .msg_name = &addr,
            .msg_namelen = sizeof(ping->addr),
            .msg_iov = &iov,
            .msg_iovlen = 1,
        };
        const i64 bytes = recvmsg(ping->fd, &rmsg, 0);

        struct timeval end;
        gettimeofday(&end, NULL);

        if (bytes == 0) {
            dprintf(STDERR_FILENO, "%s: socket closed\n", progname);
            exit(EXIT_FAILURE);
        }

        if (bytes < 0) {
            const char* err = strerror(errno);
            dprintf(STDERR_FILENO, "%s: %s\n", progname, err);
            exit(EXIT_FAILURE);
        }

        Packet r_pkt;
        u32 ip_hdr_size = 0;

        const bool receive_success = decode_msg(buffer, bytes, &r_pkt, &ip_hdr_size);

        char src_ip[INET_ADDRSTRLEN] = { 0 };
        char addrname[NI_MAXHOST] = { 0 };

        struct sockaddr_in* src_addr = rmsg.msg_name;
        inet_ntop(AF_INET, &src_addr->sin_addr.s_addr, src_ip, sizeof(src_ip));
        const bool dns_lookup_success = dns_lookup(*src_addr, addrname, sizeof(addrname));

        if (!receive_success) {
            switch (r_pkt.header.type) {
                case Icmp_TimeExceeded:
                    printf("%lu bytes from ", bytes - ip_hdr_size);
                    if (!options.no_dns && dns_lookup_success) {
                        printf("%s (%s): ", addrname, src_ip);
                    } else {
                        printf("%s: ", src_ip);
                    }
                    printf("Time to live exceeded\n");
                    break;
                case Icmp_EchoReply:
                    printf("checksum mismatch\n");
                    break;
                case Icmp_EchoRequest:
                    // from localhost
                    pkt_transmitted--;
                    continue;
                    break;
                default:
                    printf("unknown error\n");
                    break;
            }

            goto next_ping;
        }

        pkt_received++;

        const double time = to_ms(time_diff(end, start));

        printf("%lu bytes from ", bytes - ip_hdr_size);

        if (!options.no_dns && dns_lookup(*src_addr, addrname, sizeof(addrname))) {
            printf("%s (%s): ", addrname, src_ip);
        } else {
            printf("%s: ", src_ip);
        }

        printf("icmp_seq=%u ttl=%u time=%.2lf ms\n", ntohs(r_pkt.header.seq), TTL, time);

    next_ping:
        usleep(1000 * 1000);
    }

    printf("--- %s ping statistics ---\n", ping->dst);
    printf(
        "%u packets transmitted, %u received, %u%% packet loss\n",
        pkt_transmitted,
        pkt_received,
        (u16)((float)(pkt_transmitted - pkt_received) / pkt_transmitted * 100.0)
    );
}

static void
invalid_argument(const char* arg) {
    dprintf(STDERR_FILENO, "%s: invalid argument: '%s'\n", progname, arg);
}

static Options
parse_options(const i32 argc, const char* const* argv) {
    if (argc < 2) {
        usage();
        exit(EXIT_FAILURE);
    }

    Options out = { 0 };

    bool next_arg = false;

    for (i32 i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            for (i32 j = 1; argv[i][j]; j++) {
                switch (argv[i][j]) {
                    case 'v':
                        out.verbose = true;
                        break;
                    case 'h':
                        out.help = true;
                        break;
                    case 'n':
                        out.no_dns = true;
                        break;
                    case 't': {
                        out.ttl = true;
                        out.ttl_value = -1;
                        if (argv[i][j + 1]) {
                            out.ttl_value = ft_atoi(&argv[i][j + 1]);
                            if (out.ttl_value == -1) {
                                invalid_argument(&argv[i][j + 1]);
                                exit(EXIT_FAILURE);
                            }
                            goto next;
                        } else if (i + 1 != argc) {
                            out.ttl_value = ft_atoi(argv[i + 1]);
                            if (out.ttl_value == -1) {
                                invalid_argument(argv[i + 1]);
                                exit(EXIT_FAILURE);
                            }
                            next_arg = true;
                            goto next;
                        } else {
                            usage();
                            exit(EXIT_FAILURE);
                        }
                        break;
                    }
                    default:
                        dprintf(STDERR_FILENO, "%s: invalid flag: '%c'\n", progname, argv[i][j]);
                        exit(EXIT_FAILURE);
                        break;
                }
            }
        } else if (out.dst != NULL) {
            usage();
            exit(EXIT_FAILURE);
        } else {
            out.dst = argv[i];
        }

    next:
        if (next_arg) {
            i++;
        }
        next_arg = false;
    }

    if (out.dst == NULL) {
        dprintf(STDERR_FILENO, "%s: usage error: destination address required\n", progname);
        exit(EXIT_FAILURE);
    }

    return out;
}

int
main(int argc, const char* const* argv) {
    progname = argc > 0 ? argv[0] : "ft_ping";
    options = parse_options(argc, argv);

    if (options.help) {
        usage();
        exit(EXIT_SUCCESS);
    }

    if (options.ttl) {
        TTL = options.ttl_value;
    }

    const bool is_root = getuid() == 0;

    PingData ping = {
        .dst = options.dst,
    };

    ping.is_ip_format = is_ipv4(ping.dst);
    ping.addr = lookup_addr(ping.dst);
    inet_ntop(AF_INET, &ping.addr.sin_addr.s_addr, ping.ip, INET_ADDRSTRLEN);
    dns_lookup(ping.addr, ping.host, sizeof(ping.host));

    ping.fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);

    if (ping.fd < 0) {
        if (!is_root && (errno == EPERM || errno == EACCES)) {
            dprintf(STDERR_FILENO, "%s: lacking priviledge for icmp socket\n", progname);
        } else {
            const char* err = strerror(errno);
            dprintf(STDERR_FILENO, "%s: %s\n", progname, err);
        }
        exit(EXIT_FAILURE);
    }

    signal(SIGINT, int_handler);

    send_ping(&ping);

    close(ping.fd);
}

// TODO: check for dupes (packets)
// TODO: SIGALARM
