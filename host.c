#define _POSIX_C_SOURCE 200809L
#include "frame.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>

static atomic_int g_stop = 0;
static int        g_sock = -1;

static int opt_trickle       = 0;
static int opt_die_mid_frame = 0;
static int opt_no_read       = 0;
static int opt_slow_read_ms  = 0;
static int opt_coalesce      = 0;
static int opt_sniffer       = 0;
static int opt_flood_n       = 0;

static int parse_mac(const char *str, uint8_t mac[MAC_LEN])
{
    unsigned a, b, c, d, e, f;
    if (sscanf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
               &a, &b, &c, &d, &e, &f) != 6) {
        return -1;
    }
    mac[0] = (uint8_t)a; mac[1] = (uint8_t)b; mac[2] = (uint8_t)c;
    mac[3] = (uint8_t)d; mac[4] = (uint8_t)e; mac[5] = (uint8_t)f;
    return 0;
}

static void mac_to_str(const uint8_t mac[MAC_LEN], char *out)
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static int send_frame(int sock, const frame_header_t *hdr,
                      const uint8_t *payload, size_t plen)
{
    uint8_t hdr_buf[FRAME_HEADER_SIZE];
    frame_header_serialize(hdr, hdr_buf);

    size_t total = FRAME_HEADER_SIZE + plen;
    uint8_t *buf = malloc(total);
    if (!buf) { perror("send_frame malloc"); return -1; }
    memcpy(buf, hdr_buf, FRAME_HEADER_SIZE);
    if (plen > 0 && payload) memcpy(buf + FRAME_HEADER_SIZE, payload, plen);

    ssize_t r;
    if (opt_trickle) {
        for (size_t i = 0; i < total; i++) {
            r = write(sock, buf + i, 1);
            if (r < 0) { free(buf); return -1; }
        }
    } else if (opt_die_mid_frame) {
        size_t half = total / 2;
        if (half > 0) write(sock, buf, half);
        free(buf);
        atomic_store(&g_stop, 1);
        return 0;
    } else if (opt_coalesce) {
        uint8_t *big = malloc(total * 2);
        if (!big) { free(buf); return -1; }
        memcpy(big, buf, total);
        memcpy(big + total, buf, total);
        r = write(sock, big, total * 2);
        free(big);
        free(buf);
        if (r < 0) return -1;
        return 0;
    } else {
        r = write(sock, buf, total);
        free(buf);
        if (r < 0) return -1;
        return 0;
    }
    free(buf);
    return 0;
}

static void *reader_thread_fn(void *arg)
{
    (void)arg;

    if (opt_no_read) {
        while (!atomic_load(&g_stop)) {
            struct timespec ts = {0, 50000000L};
            nanosleep(&ts, NULL);
        }
        return NULL;
    }

    while (!atomic_load(&g_stop)) {
        if (opt_slow_read_ms > 0) {
            struct pollfd pfd;
            pfd.fd     = g_sock;
            pfd.events = POLLIN;
            int r = poll(&pfd, 1, opt_slow_read_ms);
            if (r <= 0) continue;
        }

        uint8_t hdr_buf[FRAME_HEADER_SIZE];
        size_t done = 0;
        while (done < FRAME_HEADER_SIZE) {
            struct pollfd pfd;
            pfd.fd = g_sock; pfd.events = POLLIN;
            int pr = poll(&pfd, 1, 200);
            if (pr < 0) { if (errno == EINTR) continue; goto exit_reader; }
            if (pr == 0) {
                if (atomic_load(&g_stop)) goto exit_reader;
                continue;
            }
            ssize_t nr = read(g_sock, hdr_buf + done, FRAME_HEADER_SIZE - done);
            if (nr <= 0) {
                if (done >= 11 && memcmp(hdr_buf, "SWITCH FULL", 11) == 0) {
                    printf("SWITCH FULL\n");
                    fflush(stdout);
                }
                goto exit_reader;
            }
            done += (size_t)nr;
            if (done >= 11 && memcmp(hdr_buf, "SWITCH FULL", 11) == 0) {
                printf("SWITCH FULL\n");
                fflush(stdout);
                goto exit_reader;
            }
        }

        frame_header_t hdr;
        frame_header_deserialize(hdr_buf, &hdr);

        uint8_t *payload = NULL;
        if (hdr.length > 0) {
            payload = malloc(hdr.length + 1);
            if (!payload) goto exit_reader;
            done = 0;
            while (done < hdr.length) {
                struct pollfd pfd;
                pfd.fd = g_sock; pfd.events = POLLIN;
                int pr = poll(&pfd, 1, 500);
                if (pr < 0) { if (errno == EINTR) continue; free(payload); goto exit_reader; }
                if (pr == 0) continue;
                ssize_t nr = read(g_sock, payload + done, hdr.length - done);
                if (nr <= 0) { free(payload); goto exit_reader; }
                done += (size_t)nr;
            }
            payload[hdr.length] = '\0';
        }

        char src_str[18], dst_str[18];
        mac_to_str(hdr.src, src_str);
        mac_to_str(hdr.dst, dst_str);
        if (opt_sniffer) {
            printf("SNIFF %s %s v=%u p=%u len=%u %s\n",
                   src_str, dst_str, hdr.vlan, hdr.priority, hdr.length,
                   payload ? (char *)payload : "");
        } else {
            printf("RX %s %s v=%u p=%u len=%u %s\n",
                   src_str, dst_str, hdr.vlan, hdr.priority, hdr.length,
                   payload ? (char *)payload : "");
        }
        fflush(stdout);
        free(payload);
    }

exit_reader:
    atomic_store(&g_stop, 1);
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <switch_ip> <tcp_port> <mac> [options]\n",
                argv[0]);
        return 1;
    }

    const char *switch_ip  = argv[1];
    const char *port_str   = argv[2];
    const char *mac_str    = argv[3];

    uint8_t my_mac[MAC_LEN];
    if (parse_mac(mac_str, my_mac) != 0) {
        fprintf(stderr, "Bad MAC: %s\n", mac_str);
        return 1;
    }

    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--trickle") == 0) {
            opt_trickle = 1;
        } else if (strcmp(argv[i], "--die-mid-frame") == 0) {
            opt_die_mid_frame = 1;
        } else if (strcmp(argv[i], "--no-read") == 0) {
            opt_no_read = 1;
        } else if (strcmp(argv[i], "--slow-read") == 0 && i + 1 < argc) {
            opt_slow_read_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--coalesce") == 0) {
            opt_coalesce = 1;
        } else if (strcmp(argv[i], "--sniffer") == 0) {
            opt_sniffer = 1;
        } else if (strcmp(argv[i], "--flood") == 0 && i + 1 < argc) {
            opt_flood_n = atoi(argv[++i]);
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
        }
    }

    char *endp = NULL;
    long lport = strtol(port_str, &endp, 10);
    if (endp == port_str || *endp != '\0' || lport < 1 || lport > 65535) {
        fprintf(stderr, "Bad port: %s\n", port_str);
        return 1;
    }

    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock < 0) { perror("socket"); return 1; }

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof saddr);
    saddr.sin_family = AF_INET;
    saddr.sin_port   = htons((uint16_t)lport);
    if (inet_pton(AF_INET, switch_ip, &saddr.sin_addr) <= 0) {
        fprintf(stderr, "Bad IP: %s\n", switch_ip);
        return 1;
    }

    if (connect(g_sock, (struct sockaddr *)&saddr, sizeof saddr) != 0) {
        perror("connect");
        return 1;
    }

    pthread_t rtid;
    int rc = pthread_create(&rtid, NULL, reader_thread_fn, NULL);
    if (rc != 0) {
        fprintf(stderr, "pthread_create reader: %s\n", strerror(rc));
        return 1;
    }

    if (opt_flood_n > 0) {
        uint8_t bcast[MAC_LEN];
        memset(bcast, 0xFF, MAC_LEN);
        frame_header_t fhdr;
        memset(&fhdr, 0, sizeof fhdr);
        memcpy(fhdr.src, my_mac, MAC_LEN);
        memcpy(fhdr.dst, bcast, MAC_LEN);
        fhdr.vlan = 1;
        fhdr.priority = PRIO_NORMAL;
        const char *ftext = "flood";
        fhdr.length = (uint16_t)strlen(ftext);
        for (int fi = 0; fi < opt_flood_n && !atomic_load(&g_stop); fi++) {
            if (send_frame(g_sock, &fhdr, (const uint8_t *)ftext, strlen(ftext)) != 0) break;
        }
        atomic_store(&g_stop, 1);
        shutdown(g_sock, SHUT_RDWR);
        pthread_join(rtid, NULL);
        rc = close(g_sock);
        if (rc != 0) perror("host: close sock");
        return 0;
    }

    char line[2048];
    while (!atomic_load(&g_stop) && fgets(line, sizeof line, stdin) != NULL) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        char cmd[16];
        if (sscanf(line, "%15s", cmd) != 1) continue;

        if (strcmp(cmd, "quit") == 0) {
            atomic_store(&g_stop, 1);
            break;
        }

        if (strcmp(cmd, "send") == 0 || strcmp(cmd, "bcast") == 0 ||
            strcmp(cmd, "spoof") == 0) {
            char src_str[18] = "";
            char dst_str[18] = "ff:ff:ff:ff:ff:ff";
            int  prio = 1, vlan = 1;
            char text[1500] = "";

            if (strcmp(cmd, "spoof") == 0) {
                if (sscanf(line, "%*s %17s %17s %d %d %1499[^\n]",
                           src_str, dst_str, &prio, &vlan, text) < 4) {
                    fprintf(stderr, "Usage: spoof <src> <dst> <prio> <vlan> [text]\n");
                    continue;
                }
            } else if (strcmp(cmd, "send") == 0) {
                if (sscanf(line, "%*s %17s %d %d %1499[^\n]",
                           dst_str, &prio, &vlan, text) < 3) {
                    fprintf(stderr, "Usage: send <dst> <prio> <vlan> [text]\n");
                    continue;
                }
            } else {
                if (sscanf(line, "%*s %d %d %1499[^\n]",
                           &prio, &vlan, text) < 2) {
                    fprintf(stderr, "Usage: bcast <prio> <vlan> [text]\n");
                    continue;
                }
            }

            uint8_t use_src[MAC_LEN];
            memcpy(use_src, my_mac, MAC_LEN);
            if (strcmp(cmd, "spoof") == 0) {
                if (parse_mac(src_str, use_src) != 0) {
                    fprintf(stderr, "Bad src MAC: %s\n", src_str);
                    continue;
                }
            }

            uint8_t dst_mac[MAC_LEN];
            if (strcmp(cmd, "bcast") == 0) {
                memset(dst_mac, 0xFF, MAC_LEN);
            } else {
                if (parse_mac(dst_str, dst_mac) != 0) {
                    fprintf(stderr, "Bad dst MAC: %s\n", dst_str);
                    continue;
                }
            }

            frame_header_t hdr;
            memset(&hdr, 0, sizeof hdr);
            memcpy(hdr.src, use_src, MAC_LEN);
            memcpy(hdr.dst, dst_mac, MAC_LEN);
            hdr.vlan     = (uint16_t)(vlan > 0 ? vlan : 1);
            hdr.priority = (uint8_t)(prio >= 0 && prio <= 2 ? prio : 1);
            size_t tlen  = strlen(text);
            hdr.length   = (uint16_t)tlen;

            if (send_frame(g_sock, &hdr, (uint8_t *)text, tlen) != 0) {
                perror("send_frame");
            }
            if (opt_die_mid_frame) break;
        }
    }

    if (!atomic_load(&g_stop)) {
        pthread_join(rtid, NULL);
        int rc2 = close(g_sock);
        if (rc2 != 0) perror("host: close sock");
        return 0;
    }

    atomic_store(&g_stop, 1);
    shutdown(g_sock, SHUT_RDWR);
    pthread_join(rtid, NULL);
    int rc2 = close(g_sock);
    if (rc2 != 0) perror("host: close sock");
    return 0;
}
