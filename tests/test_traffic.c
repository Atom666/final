#include <assert.h>
#include <string.h>
#include <mirror/test_traffic.h>

int main(void) {
    uint8_t packet[1400]; struct mirror_test_packet parsed;
    assert(mirror_test_build_packet(packet, sizeof(packet), 1200, 77, 1) == 0);
    assert(mirror_test_parse_packet(packet, 1200, &parsed) == 0);
    assert(parsed.run_id == 77 && parsed.sequence == 1 && parsed.packet_length == 1200);
    packet[100] ^= 1;
    assert(mirror_test_parse_packet(packet, 1200, &parsed) == -2);
    packet[100] ^= 1;
    assert(mirror_test_build_packet(packet, sizeof(packet), 39, 77, 1) != 0);
    assert(mirror_test_build_packet(packet, sizeof(packet), 1200, 77, 0) != 0);

    struct mirror_test_tracker tracker;
    assert(mirror_test_tracker_init(&tracker, 3) == 0);
    for (uint64_t sequence = 1; sequence <= 3; sequence++) {
        assert(mirror_test_build_packet(packet, sizeof(packet), 100, 77, sequence) == 0);
        mirror_test_tracker_record(&tracker, packet, 100, 77);
    }
    assert(mirror_test_tracker_success(&tracker));
    mirror_test_tracker_record(&tracker, packet, 100, 77);
    assert(tracker.duplicate_packets == 1 && !mirror_test_tracker_success(&tracker));
    mirror_test_tracker_destroy(&tracker);

    assert(mirror_test_tracker_init(&tracker, 3) == 0);
    assert(mirror_test_build_packet(packet, sizeof(packet), 100, 77, 1) == 0);
    mirror_test_tracker_record(&tracker, packet, 100, 77);
    assert(mirror_test_tracker_missing(&tracker) == 2);
    packet[50] ^= 1; mirror_test_tracker_record(&tracker, packet, 100, 77);
    assert(tracker.corrupted_packets == 1);
    assert(mirror_test_build_packet(packet, sizeof(packet), 100, 88, 1) == 0);
    mirror_test_tracker_record(&tracker, packet, 100, 77);
    assert(tracker.matching_packets == 2);
    mirror_test_tracker_destroy(&tracker);
    return 0;
}
