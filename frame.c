#define _POSIX_C_SOURCE 200809L
#include "frame.h"

#include <arpa/inet.h>
#include <string.h>

int frame_header_serialize(const frame_header_t *hdr,uint8_t out[FRAME_HEADER_SIZE])
{
    if (hdr == NULL || out == NULL) {
        return -1;
    }
    uint16_t len_n  = htons(hdr->length);
    uint16_t vlan_n = htons(hdr->vlan);
    memcpy(out + 0,  &len_n, 2);
    memcpy(out + 2,  hdr->src, MAC_LEN);
    memcpy(out + 8,  hdr->dst, MAC_LEN);
    memcpy(out + 14, &vlan_n, 2);
    out[16] = hdr->priority;
    return 0;
}

int frame_header_deserialize(const uint8_t in[FRAME_HEADER_SIZE],
                             frame_header_t *hdr)
{
    if (hdr == NULL || in == NULL) {
        return -1;
    }
    uint16_t len_n;
    uint16_t vlan_n;
    memcpy(&len_n, in + 0, 2);
    memcpy(hdr->src, in + 2, MAC_LEN);
    memcpy(hdr->dst, in + 8, MAC_LEN);
    memcpy(&vlan_n, in + 14, 2);
    hdr->length   = ntohs(len_n);
    hdr->vlan     = ntohs(vlan_n);
    hdr->priority = in[16];
    return 0;
}

int frame_header_validate(const frame_header_t *hdr)
{
    if (hdr == NULL) {
        return -1;
    }
    if (hdr->length > FRAME_MAX_PAYLOAD) {
        return -1;
    }
    if (hdr->vlan < FRAME_VLAN_MIN || hdr->vlan > FRAME_VLAN_MAX) {
        return -1;
    }
    if (hdr->priority > PRIO_LOW) {
        return -1;
    }
    return 0;
}

int frame_mac_is_broadcast(const uint8_t mac[MAC_LEN])
{
    for (int i = 0; i < MAC_LEN; i++) {
        if (mac[i] != 0xFF) {
            return 0;
        }
    }
    return 1;
}
