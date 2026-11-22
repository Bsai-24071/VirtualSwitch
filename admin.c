#define _POSIX_C_SOURCE 200809L
#include "switch.h"
#include "port.h"
#include "common.h"
#include "mac_table.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static void admin_send(int fd, const char *msg)
{
    size_t len = strlen(msg);
    size_t done = 0;
    while (done < len) {
        ssize_t w = write(fd, msg + done, len - done);
        if (w <= 0) return;
        done += (size_t)w;
    }
}

static void handle_client(switch_t *sw, int cfd)
{
    char buf[512];
    int pos = 0;
    for (;;) {
        ssize_t nr = read(cfd, buf + pos, sizeof buf - 1 - (size_t)pos);
        if (nr <= 0) return;
        pos += (int)nr;
        buf[pos] = '\0';
        if (strchr(buf, '\n')) break;
        if (pos >= (int)sizeof buf - 2) break;
    }
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    char *cr = strchr(buf, '\r');
    if (cr) *cr = '\0';

    char cmd[64] = "";
    sscanf(buf, "%63s", cmd);

    if (strcmp(cmd, "show") == 0) {
        char sub[32] = "";
        sscanf(buf, "%*s %31s", sub);

        if (strcmp(sub, "mac") == 0) {
            char tmp_path[256];
            snprintf(tmp_path, sizeof tmp_path, "/tmp/vswitch_dump_%d.txt", (int)getpid());
            FILE *f = fopen(tmp_path, "w");
            if (!f) {
                admin_send(cfd, "ERR cannot open dump file\n");
                return;
            }
            mac_table_dump(&sw->mac_table, f);
            if (fclose(f) != 0) {
                perror("admin: show mac fclose");
            }
            f = fopen(tmp_path, "r");
            if (!f) {
                admin_send(cfd, "ERR cannot read dump file\n");
                return;
            }
            admin_send(cfd, "OK\n");
            char line[256];
            while (fgets(line, sizeof line, f)) {
                admin_send(cfd, line);
            }
            if (fclose(f) != 0) {
                perror("admin: show mac fclose2");
            }
            remove(tmp_path);
        } else if (strcmp(sub, "ports") == 0) {
            admin_send(cfd, "OK\n");
            char line[128];
            for (int i = 0; i < sw->num_ports; i++) {
                int st = atomic_load_explicit(&sw->ports[i].state, memory_order_relaxed);
                int vlan = atomic_load_explicit(&sw->ports[i].vlan, memory_order_relaxed);
                int mirror = atomic_load_explicit(&sw->ports[i].mirror_to, memory_order_relaxed);
                int trunk = atomic_load_explicit(&sw->ports[i].trunk, memory_order_relaxed);
                const char *state_str = "unknown";
                switch (st) {
                    case PORT_FREE:          state_str = "free"; break;
                    case PORT_STARTING:      state_str = "starting"; break;
                    case PORT_ACTIVE:        state_str = "active"; break;
                    case PORT_CLOSING:       state_str = "closing"; break;
                    case PORT_ADMIN_DOWN:    state_str = "admin_down"; break;
                    case PORT_SECURITY_DOWN: state_str = "security_down"; break;
                }
                snprintf(line, sizeof line,
                         "port=%d state=%s vlan=%d mirror=%d trunk=%d\n",
                         i, state_str, vlan, mirror, trunk);
                admin_send(cfd, line);
            }
        } else {
            admin_send(cfd, "ERR unknown show subcommand\n");
        }
    } else if (strcmp(cmd, "shutdown") == 0) {
        int idx = -1;
        if (sscanf(buf, "%*s %d", &idx) != 1 || idx < 0 || idx >= sw->num_ports) {
            admin_send(cfd, "ERR bad port index\n");
            return;
        }
        port_shutdown(sw, idx);
        admin_send(cfd, "OK\n");
    } else if (strcmp(cmd, "up") == 0) {
        int idx = -1;
        if (sscanf(buf, "%*s %d", &idx) != 1 || idx < 0 || idx >= sw->num_ports) {
            admin_send(cfd, "ERR bad port index\n");
            return;
        }
        port_up(sw, idx);
        admin_send(cfd, "OK\n");
    } else if (strcmp(cmd, "vlan") == 0) {
        int idx = -1, vlan = -1;
        if (sscanf(buf, "%*s %d %d", &idx, &vlan) != 2 ||
            idx < 0 || idx >= sw->num_ports ||
            vlan < 1 || vlan > 4094) {
            admin_send(cfd, "ERR bad port or vlan\n");
            return;
        }
        atomic_store_explicit(&sw->ports[idx].vlan, vlan, memory_order_relaxed);
        int rc = pthread_mutex_lock(&sw->cfg_lock);
        if (rc == 0) {
            if (idx < 64) sw->cfg.vlan[idx] = vlan;
            int rc2 = pthread_mutex_unlock(&sw->cfg_lock);
            if (rc2 != 0) fprintf(stderr, "admin: vlan unlock: %s\n", strerror(rc2));
        }
        mac_table_remove_port(&sw->mac_table, idx);
        admin_send(cfd, "OK\n");
    } else if (strcmp(cmd, "mirror") == 0) {
        int idx = -1, sniffer = -1;
        if (sscanf(buf, "%*s %d %d", &idx, &sniffer) != 2 ||
            idx < 0 || idx >= sw->num_ports ||
            sniffer < -1 || sniffer >= sw->num_ports) {
            admin_send(cfd, "ERR bad port or sniffer\n");
            return;
        }
        if (idx == sniffer) {
            admin_send(cfd, "ERR cannot mirror to self\n");
            return;
        }
        atomic_store_explicit(&sw->ports[idx].mirror_to, sniffer, memory_order_relaxed);
        admin_send(cfd, "OK\n");
    } else if (strcmp(cmd, "clear") == 0) {
        char sub[32] = "";
        sscanf(buf, "%*s %31s", sub);
        if (strcmp(sub, "mac") == 0) {
            mac_table_clear(&sw->mac_table);
            admin_send(cfd, "OK\n");
        } else {
            admin_send(cfd, "ERR unknown clear subcommand\n");
        }
    } else if (strcmp(cmd, "stats") == 0) {
        admin_send(cfd, "OK\n");
        char line[128];
        for (int i = 0; i < sw->num_ports; i++) {
            long in = 0, out = 0, drop = 0, mirror = 0;
            stats_snapshot(i, &in, &out, &drop, &mirror);
            snprintf(line, sizeof line,
                     "port=%d in=%ld out=%ld drop=%ld mirror=%ld\n",
                     i, in, out, drop, mirror);
            admin_send(cfd, line);
        }
    } else {
        admin_send(cfd, "ERR unknown command\n");
    }
}

void *admin_thread_fn(void *arg)
{
    switch_t *sw = (switch_t *)arg;

    char sock_path[256];
    int rc = pthread_mutex_lock(&sw->cfg_lock);
    if (rc == 0) {
        strncpy(sock_path, sw->cfg.sock_path, sizeof sock_path - 1);
        sock_path[sizeof sock_path - 1] = '\0';
        int rc2 = pthread_mutex_unlock(&sw->cfg_lock);
        if (rc2 != 0) fprintf(stderr, "admin: cfg unlock: %s\n", strerror(rc2));
    } else {
        strncpy(sock_path, "/tmp/vswitch.sock", sizeof sock_path - 1);
        sock_path[sizeof sock_path - 1] = '\0';
    }

    unlink(sock_path);

    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) {
        perror("admin: socket");
        return NULL;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof addr.sun_path - 1);

    if (bind(sfd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        perror("admin: bind");
        rc = close(sfd);
        if (rc != 0) perror("admin: close sfd");
        return NULL;
    }

    if (listen(sfd, 8) != 0) {
        perror("admin: listen");
        rc = close(sfd);
        if (rc != 0) perror("admin: close sfd");
        unlink(sock_path);
        return NULL;
    }

    int flags = fcntl(sfd, F_GETFL, 0);
    if (flags >= 0) fcntl(sfd, F_SETFL, flags | O_NONBLOCK);

    for (;;) {
        struct pollfd fds[2];
        fds[0].fd = sfd;
        fds[0].events = POLLIN;
        fds[1].fd = sw->admin_pipe[0];
        fds[1].events = POLLIN;

        int r = poll(fds, 2, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("admin: poll");
            break;
        }

        if (fds[1].revents & POLLIN) break;
        if (!(fds[0].revents & POLLIN)) continue;

        int cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            perror("admin: accept");
            break;
        }

        handle_client(sw, cfd);
        rc = close(cfd);
        if (rc != 0) perror("admin: close cfd");
    }

    rc = close(sfd);
    if (rc != 0) perror("admin: close sfd");
    unlink(sock_path);
    return NULL;
}
