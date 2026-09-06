#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <receiver/dedup.h>

static struct packet_fingerprint fp(unsigned value) {
    struct packet_fingerprint f = { .format_version = 1, .ip_protocol = 6, .payload_length = 4 };
    f.bytes[0] = (uint8_t)value; f.payload_hash[0] = (uint8_t)(value + 1); return f;
}

struct worker_arg { struct dedup_cache *cache; uint8_t agent[16]; unsigned base; };

struct submit_probe { unsigned calls; int result; };

static int probe_submit(void *context) {
    struct submit_probe *probe = context;
    probe->calls++;
    return probe->result;
}

static void *cache_worker(void *argp) {
    struct worker_arg *arg = argp;
    for (unsigned i = 0; i < 2000; i++) {
        struct packet_fingerprint f = fp(arg->base + i);
        uint64_t now = (uint64_t)i + 1;
        dedup_cache_check_and_add(arg->cache, &f, arg->agent, now, true, now, now);
    }
    return NULL;
}

int main(void) {
    struct receiver_config cfg; receiver_config_defaults(&cfg);
    cfg.dedup_shards = 1; cfg.dedup_max_entries = 2; cfg.dedup_window_ms = 100;
    cfg.dedup_timestamp_tolerance_ms = 10;
    struct dedup_cache cache; char err[128];
    assert(dedup_cache_init(&cache, &cfg, err, sizeof(err)) == 0);
    uint8_t a[16] = {1}, b[16] = {2}; struct packet_fingerprint f = fp(1);
    assert(dedup_cache_check_and_add(&cache, &f, a, 1000, true, 1000, 1) == DEDUP_PASS);
    assert(dedup_cache_check_and_add(&cache, &f, b, 1001, true, 1001, 2) == DEDUP_CROSS_AGENT_DUPLICATE);
    assert(dedup_cache_check_and_add(&cache, &f, a, 1002, true, 1002, 3) == DEDUP_PASS);
    assert(dedup_cache_check_and_add(&cache, &f, b, 1003, true, 1003, 4) == DEDUP_CROSS_AGENT_DUPLICATE);
    struct packet_fingerprint far = fp(9);
    assert(dedup_cache_check_and_add(&cache, &far, a, 1, true, 1, 4) == DEDUP_PASS);
    assert(dedup_cache_check_and_add(&cache, &far, b, 20000000, true, 20000000, 4) == DEDUP_PASS);
    assert(dedup_cache_check_and_add(&cache, &f, a, 1004, true, 1004, 5) == DEDUP_PASS);
    assert(dedup_cache_check_and_add(&cache, &f, a, 1005, true, 1005, 6) == DEDUP_PASS);
    assert(__atomic_load_n(&cache.entries, __ATOMIC_RELAXED) == 2);
    struct packet_fingerprint f2 = fp(2);
    assert(dedup_cache_check_and_add(&cache, &f2, a, 2000, true, 2000, 7) == DEDUP_PASS);
    assert(__atomic_load_n(&cache.evictions, __ATOMIC_RELAXED) > 0);
    uint64_t entries_before_invalid = __atomic_load_n(&cache.entries, __ATOMIC_RELAXED);
    assert(dedup_cache_check_and_add(&cache, &f2, b, 2001, false, 0, 8) == DEDUP_PASS);
    assert(__atomic_load_n(&cache.entries, __ATOMIC_RELAXED) == entries_before_invalid);
    assert(dedup_cache_check_and_add(&cache, &f2, b, 2001, true, 2001, 200000009) == DEDUP_PASS);
    assert(__atomic_load_n(&cache.expired, __ATOMIC_RELAXED) > 0);
    dedup_cache_destroy(&cache);

    cfg.dedup_shards = 1; cfg.dedup_max_entries = 8; cfg.dedup_window_ms = 100;
    assert(dedup_cache_init(&cache, &cfg, err, sizeof(err)) == 0);
    struct submit_probe rejected = {.result = -1};
    assert(dedup_cache_submit(&cache, &f, a, 3000, true, 3000, 1,
                              probe_submit, &rejected) == DEDUP_SUBMIT_REJECTED);
    assert(rejected.calls == 1);
    assert(__atomic_load_n(&cache.entries, __ATOMIC_RELAXED) == 0);
    struct submit_probe accepted = {0};
    assert(dedup_cache_submit(&cache, &f, b, 3001, true, 3001, 2,
                              probe_submit, &accepted) == DEDUP_SUBMIT_ACCEPTED);
    assert(accepted.calls == 1);
    assert(__atomic_load_n(&cache.entries, __ATOMIC_RELAXED) == 1);
    assert(__atomic_load_n(&cache.peer_rescued_after_reject, __ATOMIC_RELAXED) == 1);
    struct submit_probe duplicate = {0};
    assert(dedup_cache_submit(&cache, &f, a, 3002, true, 3002, 3,
                              probe_submit, &duplicate) ==
                              DEDUP_SUBMIT_CROSS_AGENT_DUPLICATE);
    assert(duplicate.calls == 0);
    assert(__atomic_load_n(&cache.entries, __ATOMIC_RELAXED) == 0);
    struct packet_fingerprint rejected_pair_fp = fp(77);
    struct submit_probe rejected_a = {.result = -1};
    struct submit_probe rejected_b = {.result = -1};
    assert(dedup_cache_submit(&cache, &rejected_pair_fp, a, 4000, true, 4000, 4,
                              probe_submit, &rejected_a) == DEDUP_SUBMIT_REJECTED);
    assert(dedup_cache_submit(&cache, &rejected_pair_fp, b, 4001, true, 4001, 5,
                              probe_submit, &rejected_b) == DEDUP_SUBMIT_REJECTED);
    assert(__atomic_load_n(&cache.rejected_observation_pairs, __ATOMIC_RELAXED) == 1);
    assert(__atomic_load_n(&cache.rejected_entries, __ATOMIC_RELAXED) == 0);
    dedup_cache_destroy(&cache);

    cfg.clock_offset_count = 2; memcpy(cfg.clock_offsets[0].agent_uuid, a, 16);
    cfg.clock_offsets[0].offset_ns = 50; memcpy(cfg.clock_offsets[1].agent_uuid, b, 16);
    cfg.clock_offsets[1].offset_ns = -50;
    uint64_t adjusted;
    assert(dedup_adjust_timestamp(&cfg, a, 100, &adjusted) && adjusted == 150);
    assert(dedup_adjust_timestamp(&cfg, b, 100, &adjusted) && adjusted == 50);
    assert(!dedup_adjust_timestamp(&cfg, b, 10, &adjusted));

    cfg.dedup_shards = 4; cfg.dedup_max_entries = 128; cfg.dedup_window_ms = 1000;
    assert(dedup_cache_init(&cache, &cfg, err, sizeof(err)) == 0);
    pthread_t tids[4]; struct worker_arg workers[4] = {0};
    for (unsigned i = 0; i < 4; i++) {
        workers[i].cache = &cache; workers[i].agent[0] = (uint8_t)(i + 1); workers[i].base = i * 10000;
        assert(pthread_create(&tids[i], NULL, cache_worker, &workers[i]) == 0);
    }
    for (unsigned i = 0; i < 4; i++) pthread_join(tids[i], NULL);
    assert(__atomic_load_n(&cache.entries, __ATOMIC_RELAXED) <= 128);
    dedup_cache_destroy(&cache);

    struct transport_registry tr; assert(transport_registry_init(&tr, 2) == 0);
    uint64_t gap;
    transport_registry_register(&tr, a, 10, 1);
    assert(transport_registry_check(&tr, a, 10, 0, 2, &gap) == TRANSPORT_ACCEPT);
    assert(transport_registry_check(&tr, a, 10, 0, 3, &gap) == TRANSPORT_DUPLICATE);
    assert(transport_registry_check(&tr, a, 10, 1, 4, &gap) == TRANSPORT_ACCEPT);
    assert(transport_registry_check(&tr, a, 10, 1, 5, &gap) == TRANSPORT_DUPLICATE);
    assert(transport_registry_check(&tr, a, 10, 0, 6, &gap) == TRANSPORT_OUT_OF_ORDER);
    assert(transport_registry_check(&tr, a, 11, 1, 5, &gap) == TRANSPORT_ACCEPT);
    assert(transport_registry_check(&tr, b, 10, 1, 6, &gap) == TRANSPORT_ACCEPT);
    assert(transport_registry_check(&tr, b, 10, 3, 7, &gap) == TRANSPORT_ACCEPT && gap == 1);
    transport_registry_unregister(&tr, a, 10); transport_registry_destroy(&tr);
    return 0;
}
