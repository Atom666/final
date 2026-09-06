#include <assert.h>
#include <string.h>
#include <mirror/protocol.h>

int main(void) {
    struct mirror_record_header h = {
        .magic = MIRROR_MAGIC,
        .version = MIRROR_VERSION,
        .record_type = MIRROR_RECORD_PACKET,
        .header_length = MIRROR_COMMON_HEADER_LEN,
        .payload_length = 123
    };
    uint8_t buf[MIRROR_COMMON_HEADER_LEN];
    mirror_record_header_to_wire(&h, buf);
    struct mirror_record_header out;
    mirror_record_header_from_wire(buf, &out);
    assert(out.magic == h.magic);
    assert(out.version == h.version);
    assert(out.record_type == h.record_type);
    assert(out.header_length == h.header_length);
    assert(out.payload_length == h.payload_length);
    char err[64];
    assert(mirror_validate_header(&out, 1024, err, sizeof(err)) == 0);
    assert(mirror_validate_envelope(&out, MIRROR_COMMON_HEADER_LEN + 123, 1024, err, sizeof(err)) == 0);
    assert(mirror_validate_envelope(&out, MIRROR_COMMON_HEADER_LEN + 122, 1024, err, sizeof(err)) != 0);
    out.magic = 1;
    assert(mirror_validate_header(&out, 1024, err, sizeof(err)) != 0);

    struct mirror_packet_meta m = {0}, m2 = {0};
    m.agent_uuid[0] = 0xaa;
    m.interface_id = 7;
    m.flags = MIRROR_FLAG_RX_DIRECTION | MIRROR_FLAG_VLAN_METADATA_PRESENT |
              MIRROR_FLAG_CHECKSUM_NOT_READY;
    m.connection_epoch = 0x0102030405060708ULL;
    m.sequence_number = 42;
    m.timestamp_ns = 99;
    m.captured_length = 1518;
    m.original_length = 1518;
    m.vlan_tci = 100;
    m.vlan_tpid = 0x8100;
    m.vlan_tag_count = 1;
    uint8_t mbuf[128];
    mirror_packet_meta_to_wire(&m, mbuf);
    assert(mirror_packet_meta_from_wire(mbuf, mirror_packet_meta_len(), &m2) == 0);
    assert(m2.agent_uuid[0] == 0xaa);
    assert(m2.interface_id == 7);
    assert(m2.flags & MIRROR_FLAG_CHECKSUM_NOT_READY);
    assert(m2.connection_epoch == 0x0102030405060708ULL);
    assert(m2.sequence_number == 42);
    assert(m2.vlan_tci == 100);
    assert(m2.vlan_tag_count == 1);
    struct mirror_register_payload reg = {0}, reg2 = {0};
    reg.agent_uuid[0] = 0x55; reg.interface_id = 9; reg.interface_index = 4;
    reg.interface_mtu = 1500; reg.max_capture_frame_size = 9216; reg.connection_epoch = 123456;
    strcpy(reg.hostname, "agent-test"); strcpy(reg.interface_name, "eth0");
    uint8_t rbuf[512]; mirror_register_to_wire(&reg, rbuf);
    assert(mirror_register_from_wire(rbuf, mirror_register_payload_len(), &reg2) == 0);
    assert(reg2.agent_uuid[0] == 0x55 && reg2.interface_id == 9 && reg2.connection_epoch == 123456);
    assert(strcmp(reg2.hostname, "agent-test") == 0 && strcmp(reg2.interface_name, "eth0") == 0);
    assert(mirror_register_from_wire(rbuf, mirror_register_payload_len() - 1, &reg2) != 0);
    return 0;
}
