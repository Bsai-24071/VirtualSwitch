#ifndef FRAME_H
#define FRAME_H

#include <stdint.h>
#include <stddef.h>

#define MAC_LEN             6
#define FRAME_HEADER_SIZE   17
#define FRAME_MAX_PAYLOAD   1500
#define FRAME_VLAN_MIN      1
#define FRAME_VLAN_MAX      4094

#define PRIO_HIGH           0
#define PRIO_NORMAL         1
#define PRIO_LOW            2

typedef struct __attribute__((packed)) {
    uint16_t length;
    uint8_t  src[MAC_LEN];
    uint8_t  dst[MAC_LEN];
    uint16_t vlan;
    uint8_t  priority;
} frame_header_t;

_Static_assert(sizeof(frame_header_t) == FRAME_HEADER_SIZE,"frame_header_t must be exactly 17 bytes");

int frame_header_serialize(const frame_header_t *hdr, uint8_t out[FRAME_HEADER_SIZE]);
int frame_header_deserialize(const uint8_t in[FRAME_HEADER_SIZE],frame_header_t *hdr);
int frame_header_validate(const frame_header_t *hdr);
int frame_mac_is_broadcast(const uint8_t mac[MAC_LEN]);

#endif
