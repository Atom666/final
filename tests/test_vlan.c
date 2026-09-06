#include <assert.h>
#include <string.h>
#include <mirror/vlan.h>

int main(void) {
    unsigned char frame[18] = {
        0,1,2,3,4,5, 6,7,8,9,10,11, 0x08,0x00, 0xde,0xad,0xbe,0xef
    };
    unsigned char out[32];
    size_t out_len = 0;
    assert(vlan_restore_one(frame, sizeof(frame), 0x8100, 100, out, sizeof(out), &out_len) == 0);
    assert(out_len == sizeof(frame) + 4);
    assert(memcmp(out, frame, 12) == 0);
    assert(out[12] == 0x81 && out[13] == 0x00);
    assert(out[14] == 0x00 && out[15] == 100);
    assert(out[16] == 0x08 && out[17] == 0x00);
    assert(memcmp(out + 18, frame + 14, 4) == 0);
    uint16_t tpids[2] = {0x88a8, 0x8100}, tcis[2] = {10, 100};
    assert(vlan_restore_tags(frame, sizeof(frame), tpids, tcis, 2, out, sizeof(out), &out_len) == 0);
    assert(out_len == sizeof(frame) + 8);
    assert(out[12] == 0x88 && out[13] == 0xa8 && out[16] == 0x81 && out[17] == 0x00);
    assert(out[20] == 0x08 && out[21] == 0x00);
    return 0;
}
