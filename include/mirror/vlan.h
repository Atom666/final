#ifndef MIRROR_VLAN_H
#define MIRROR_VLAN_H

#include <stddef.h>
#include <stdint.h>

int vlan_restore_one(const uint8_t *frame, size_t frame_len, uint16_t vlan_tpid, uint16_t vlan_tci,
                     uint8_t *out, size_t out_cap, size_t *out_len);
int vlan_restore_tags(const uint8_t *frame, size_t frame_len, const uint16_t *vlan_tpid,
                      const uint16_t *vlan_tci, size_t tag_count,
                      uint8_t *out, size_t out_cap, size_t *out_len);

#endif
