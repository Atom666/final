#ifndef RECEIVER_DEDUP_H
#define RECEIVER_DEDUP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <mirror/config.h>
#include <receiver/fingerprint.h>

struct dedup_entry;
struct dedup_rejected_entry;

struct dedup_shard {
    pthread_mutex_t mutex;
    bool mutex_initialized;
    struct dedup_entry **buckets;
    size_t bucket_count;
    size_t capacity;
    size_t count;
    struct dedup_entry *oldest;
    struct dedup_entry *newest;
    struct dedup_rejected_entry **rejected_buckets;
    size_t rejected_bucket_count;
    size_t rejected_capacity;
    size_t rejected_count;
    struct dedup_rejected_entry *rejected_oldest;
    struct dedup_rejected_entry *rejected_newest;
};

struct dedup_cache {
    struct dedup_shard *shards;
    uint32_t shard_count;
    uint64_t ttl_ns;
    uint64_t tolerance_ns;
    uint64_t entries;
    uint64_t evictions;
    uint64_t expired;
    uint64_t rejected_entries;
    uint64_t rejected_expired;
    uint64_t peer_rescued_after_reject;
    uint64_t rejected_observation_pairs;
};

enum dedup_result { DEDUP_PASS = 0, DEDUP_CROSS_AGENT_DUPLICATE = 1 };

enum dedup_submit_result {
    DEDUP_SUBMIT_ACCEPTED = 0,
    DEDUP_SUBMIT_CROSS_AGENT_DUPLICATE = 1,
    DEDUP_SUBMIT_REJECTED = 2
};

typedef int (*dedup_submit_fn)(void *context);

int dedup_cache_init(struct dedup_cache *cache, const struct receiver_config *cfg,
                     char *err, size_t err_len);
void dedup_cache_destroy(struct dedup_cache *cache);
enum dedup_result dedup_cache_check_and_add(struct dedup_cache *cache,
        const struct packet_fingerprint *fingerprint, const uint8_t agent_uuid[16],
        uint64_t capture_timestamp_ns, bool timestamp_valid, uint64_t adjusted_timestamp_ns,
        uint64_t monotonic_now_ns);
enum dedup_submit_result dedup_cache_submit(struct dedup_cache *cache,
        const struct packet_fingerprint *fingerprint, const uint8_t agent_uuid[16],
        uint64_t capture_timestamp_ns, bool timestamp_valid, uint64_t adjusted_timestamp_ns,
        uint64_t monotonic_now_ns, dedup_submit_fn submit, void *submit_context);
bool dedup_adjust_timestamp(const struct receiver_config *cfg, const uint8_t agent_uuid[16],
                            uint64_t timestamp_ns, uint64_t *adjusted_ns);

enum transport_result {
    TRANSPORT_ACCEPT = 0,
    TRANSPORT_DUPLICATE = 1,
    TRANSPORT_OUT_OF_ORDER = 2
};

struct transport_entry {
    uint8_t agent_uuid[16];
    uint64_t epoch;
    uint64_t last_sequence;
    uint64_t last_seen_ns;
    uint32_t active;
    bool sequence_seen;
    bool used;
};

struct transport_registry {
    pthread_mutex_t mutex;
    struct transport_entry *entries;
    size_t capacity;
};

int transport_registry_init(struct transport_registry *registry, size_t capacity);
void transport_registry_destroy(struct transport_registry *registry);
void transport_registry_register(struct transport_registry *registry,
                                 const uint8_t agent_uuid[16], uint64_t epoch,
                                 uint64_t monotonic_now_ns);
void transport_registry_unregister(struct transport_registry *registry,
                                   const uint8_t agent_uuid[16], uint64_t epoch);
enum transport_result transport_registry_check(struct transport_registry *registry,
        const uint8_t agent_uuid[16], uint64_t epoch, uint64_t sequence,
        uint64_t monotonic_now_ns, uint64_t *gap);

uint64_t receiver_monotonic_ns(void);

#endif
