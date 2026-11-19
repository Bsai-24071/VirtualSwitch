#ifndef PORT_H
#define PORT_H

#include "switch.h"

int  port_init(port_t *p, int idx, int queue_capacity);
int  port_start(switch_t *sw, int idx, int rfd, int wfd);
void port_stop(switch_t *sw, int idx, int new_state);
void *reaper_thread(void *arg);

#endif
