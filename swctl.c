#define _POSIX_C_SOURCE 200809L
#include "common.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static const char *g_sock_path = "/tmp/vswitch.sock";

static int send_cmd(const char *cmd, char *resp_buf, size_t resp_sz)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("swctl: socket");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_sock_path, sizeof addr.sun_path - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        perror("swctl: connect");
        int rc = close(fd);
        if (rc != 0) perror("swctl: close");
        return -1;
    }

    size_t len = strlen(cmd);
    size_t done = 0;
    while (done < len) {
        ssize_t w = write(fd, cmd + done, len - done);
        if (w <= 0) {
            perror("swctl: write");
            int rc = close(fd);
            if (rc != 0) perror("swctl: close");
            return -1;
        }
        done += (size_t)w;
    }

    size_t rdone = 0;
    while (rdone < resp_sz - 1) {
        ssize_t nr = read(fd, resp_buf + rdone, resp_sz - 1 - rdone);
        if (nr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (nr == 0) break;
        rdone += (size_t)nr;
    }
    resp_buf[rdone] = '\0';

    int rc = close(fd);
    if (rc != 0) perror("swctl: close");
    return 0;
}

static int show_shm_stats(int watch, const char *shm_name)
{
    if (shm_stats_open_ro(shm_name) != 0) {
        fprintf(stderr, "swctl: cannot open shared memory %s\n", shm_name);
        return 1;
    }

    do {
        shm_stats_t *sh = shm_stats_ptr();
        if (!sh) break;

        printf("Port  In       Out      Drop     Mirror\n");
        printf("----  -------- -------- -------- --------\n");
        for (int i = 0; i < sh->num_ports && i < STATS_MAX_PORTS; i++) {
            printf("%-4d  %-8ld %-8ld %-8ld %-8ld\n",
                   i, sh->port[i].in, sh->port[i].out,
                   sh->port[i].drop, sh->port[i].mirror);
        }

        if (watch) {
            printf("\n");
            fflush(stdout);
            sleep(1);
        }
    } while (watch);

    shm_stats_close();
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
                "Usage: %s [--sock <path>] [--shm <name>] <command> [args]\n"
                "Commands: show mac, show ports, shutdown <port>, up <port>,\n"
                "          vlan <port> <vlan>, mirror <port> <sniffer>,\n"
                "          clear mac, stats [--watch]\n",
                argv[0]);
        return 1;
    }

    int arg_start = 1;
    const char *shm_name = "/vswitch_stats";

    while (arg_start < argc) {
        if (strcmp(argv[arg_start], "--sock") == 0 && arg_start + 1 < argc) {
            g_sock_path = argv[arg_start + 1];
            arg_start += 2;
        } else if (strcmp(argv[arg_start], "--shm") == 0 && arg_start + 1 < argc) {
            shm_name = argv[arg_start + 1];
            arg_start += 2;
        } else {
            break;
        }
    }

    if (arg_start >= argc) {
        fprintf(stderr, "swctl: no command given\n");
        return 1;
    }

    const char *cmd = argv[arg_start];

    if (strcmp(cmd, "stats") == 0) {
        int watch = 0;
        if (arg_start + 1 < argc && strcmp(argv[arg_start + 1], "--watch") == 0) {
            watch = 1;
        }
        return show_shm_stats(watch, shm_name);
    }

    char full_cmd[512];
    int clen = 0;
    for (int i = arg_start; i < argc; i++) {
        int r = snprintf(full_cmd + clen, sizeof full_cmd - (size_t)clen - 2,
                         "%s%s", i > arg_start ? " " : "", argv[i]);
        if (r > 0) clen += r;
    }
    full_cmd[clen++] = '\n';
    full_cmd[clen] = '\0';

    char resp[65536];
    if (send_cmd(full_cmd, resp, sizeof resp) != 0) {
        return 1;
    }

    fputs(resp, stdout);
    fflush(stdout);

    if (strncmp(resp, "ERR", 3) == 0) return 1;
    return 0;
}
