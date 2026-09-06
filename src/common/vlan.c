#include <mirror/vlan.h>

#include <mirror/net.h>
#include <string.h>

int vlan_restore_one(const uint8_t *frame, size_t frame_len, uint16_t vlan_tpid, uint16_t vlan_tci,
                     uint8_t *out, size_t out_cap, size_t *out_len) {
    if (frame_len < 14 || out_cap < frame_len + 4) return -1;
    memcpy(out, frame, 12);
    uint16_t tpid = htons(vlan_tpid ? vlan_tpid : 0x8100);
    uint16_t tci = htons(vlan_tci);
    memcpy(out + 12, &tpid, 2);
    memcpy(out + 14, &tci, 2);
    memcpy(out + 16, frame + 12, frame_len - 12);
    *out_len = frame_len + 4;
    return 0;
}

int vlan_restore_tags(const uint8_t *frame, size_t frame_len, const uint16_t *vlan_tpid,
                      const uint16_t *vlan_tci, size_t tag_count,
                      uint8_t *out, size_t out_cap, size_t *out_len) {
    if (!tag_count || tag_count > 4 || frame_len < 14 || out_cap < frame_len + tag_count * 4) return -1;
    memcpy(out, frame, 12); size_t pos = 12;
    for (size_t i = 0; i < tag_count; i++) {
        uint16_t tpid = htons(vlan_tpid[i] ? vlan_tpid[i] : 0x8100), tci = htons(vlan_tci[i]);
        memcpy(out + pos, &tpid, 2); memcpy(out + pos + 2, &tci, 2); pos += 4;
    }
    memcpy(out + pos, frame + 12, frame_len - 12); *out_len = frame_len + tag_count * 4; return 0;
}
