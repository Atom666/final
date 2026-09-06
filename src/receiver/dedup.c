#include <receiver/dedup.h>

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct dedup_entry {
    uint8_t fingerprint[16];
    uint8_t payload_hash[16];
    uint8_t agent_uuid[16];
    uint64_t capture_timestamp_ns;
    uint64_t adjusted_timestamp_ns;
    uint64_t receiver_monotonic_insert_ns;
    uint32_t payload_length;
    uint8_t ip_protocol;
    bool timestamp_valid;
    struct dedup_entry *hash_next;
    struct dedup_entry *older;
    struct dedup_entry *newer;
};

struct dedup_rejected_entry {
    uint8_t fingerprint[16];
    uint8_t payload_hash[16];
    uint8_t agent_uuid[16];
    uint64_t adjusted_timestamp_ns;
    uint64_t receiver_monotonic_insert_ns;
    uint32_t payload_length;
    uint8_t ip_protocol;
    struct dedup_rejected_entry *hash_next;
    struct dedup_rejected_entry *older;
    struct dedup_rejected_entry *newer;
};

uint64_t receiver_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t fingerprint_hash(const uint8_t fp[16]) {
    uint64_t a, b;
    memcpy(&a, fp, 8); memcpy(&b, fp + 8, 8);
    return a ^ (b * 0x9e3779b97f4a7c15ULL);
}

static void unlink_entry(struct dedup_cache *cache, struct dedup_shard *s,
                         size_t bucket, struct dedup_entry *entry,
                         struct dedup_entry *hash_prev) {
    if (hash_prev) hash_prev->hash_next = entry->hash_next;
    else s->buckets[bucket] = entry->hash_next;
    if (entry->older) entry->older->newer = entry->newer; else s->oldest = entry->newer;
    if (entry->newer) entry->newer->older = entry->older; else s->newest = entry->older;
    s->count--;
    __atomic_sub_fetch(&cache->entries, 1, __ATOMIC_RELAXED);
    free(entry);
}

static void remove_known(struct dedup_cache *cache, struct dedup_shard *s,
                         struct dedup_entry *entry) {
    size_t bucket = fingerprint_hash(entry->fingerprint) % s->bucket_count;
    struct dedup_entry *prev = NULL;
    for (struct dedup_entry *it = s->buckets[bucket]; it; prev = it, it = it->hash_next) {
        if (it == entry) { unlink_entry(cache, s, bucket, it, prev); return; }
    }
}

static void expire_old(struct dedup_cache *cache, struct dedup_shard *s, uint64_t now) {
    while (s->oldest && now >= s->oldest->receiver_monotonic_insert_ns &&
           now - s->oldest->receiver_monotonic_insert_ns > cache->ttl_ns) {
        struct dedup_entry *entry = s->oldest;
        remove_known(cache, s, entry);
        __atomic_add_fetch(&cache->expired, 1, __ATOMIC_RELAXED);
    }
}

static void rejected_unlink(struct dedup_cache *cache, struct dedup_shard *s,
                            size_t bucket, struct dedup_rejected_entry *entry,
                            struct dedup_rejected_entry *hash_prev) {
    if (hash_prev) hash_prev->hash_next = entry->hash_next;
    else s->rejected_buckets[bucket] = entry->hash_next;
    if (entry->older) entry->older->newer = entry->newer;
    else s->rejected_oldest = entry->newer;
    if (entry->newer) entry->newer->older = entry->older;
    else s->rejected_newest = entry->older;
    s->rejected_count--;
    __atomic_sub_fetch(&cache->rejected_entries, 1, __ATOMIC_RELAXED);
    free(entry);
}

static void rejected_remove_known(struct dedup_cache *cache, struct dedup_shard *s,
                                  struct dedup_rejected_entry *entry) {
    size_t bucket = fingerprint_hash(entry->fingerprint) % s->rejected_bucket_count;
    struct dedup_rejected_entry *prev = NULL;
    for (struct dedup_rejected_entry *it = s->rejected_buckets[bucket]; it;
         prev = it, it = it->hash_next) {
        if (it == entry) {
            rejected_unlink(cache, s, bucket, it, prev);
            return;
        }
    }
}

static void rejected_expire_old(struct dedup_cache *cache, struct dedup_shard *s,
                                uint64_t now) {
    while (s->rejected_oldest && now >= s->rejected_oldest->receiver_monotonic_insert_ns &&
           now - s->rejected_oldest->receiver_monotonic_insert_ns > cache->ttl_ns) {
        struct dedup_rejected_entry *entry = s->rejected_oldest;
        rejected_remove_known(cache, s, entry);
        __atomic_add_fetch(&cache->rejected_expired, 1, __ATOMIC_RELAXED);
    }
}

int dedup_cache_init(struct dedup_cache *cache, const struct receiver_config *cfg,
                     char *err, size_t err_len) {
    memset(cache, 0, sizeof(*cache));
    if (!cfg->dedup_enabled) return 0;
    cache->shard_count = cfg->dedup_shards;
    cache->ttl_ns = (uint64_t)cfg->dedup_window_ms * 1000000ULL;
    cache->tolerance_ns = (uint64_t)cfg->dedup_timestamp_tolerance_ms * 1000000ULL;
    cache->shards = calloc(cache->shard_count, sizeof(*cache->shards));
    if (!cache->shards) { snprintf(err, err_len, "dedup shard allocation failed"); return -1; }
    size_t base = cfg->dedup_max_entries / cache->shard_count;
    size_t remainder = cfg->dedup_max_entries % cache->shard_count;
    size_t rejected_base = cfg->dedup_rejected_max_entries / cache->shard_count;
    size_t rejected_remainder = cfg->dedup_rejected_max_entries % cache->shard_count;
    for (uint32_t i = 0; i < cache->shard_count; i++) {
        struct dedup_shard *s = &cache->shards[i];
        s->capacity = base + (i < remainder ? 1 : 0);
        s->bucket_count = s->capacity * 2 + 1;
        s->rejected_capacity = rejected_base + (i < rejected_remainder ? 1 : 0);
        s->rejected_bucket_count = s->rejected_capacity * 2 + 1;
        s->buckets = calloc(s->bucket_count, sizeof(*s->buckets));
        s->rejected_buckets = calloc(s->rejected_bucket_count,
                                     sizeof(*s->rejected_buckets));
        if (!s->buckets || !s->rejected_buckets ||
            pthread_mutex_init(&s->mutex, NULL) != 0) {
            snprintf(err, err_len, "dedup cache allocation failed at shard %u", i);
            dedup_cache_destroy(cache); return -1;
        }
        s->mutex_initialized = true;
    }
    return 0;
}

void dedup_cache_destroy(struct dedup_cache *cache) {
    if (!cache || !cache->shards) return;
    for (uint32_t i = 0; i < cache->shard_count; i++) {
        struct dedup_shard *s = &cache->shards[i];
        struct dedup_entry *it = s->oldest;
        while (it) { struct dedup_entry *next = it->newer; free(it); it = next; }
        struct dedup_rejected_entry *rejected = s->rejected_oldest;
        while (rejected) {
            struct dedup_rejected_entry *next = rejected->newer;
            free(rejected);
            rejected = next;
        }
        if (s->mutex_initialized) pthread_mutex_destroy(&s->mutex);
        free(s->buckets);
        free(s->rejected_buckets);
    }
    free(cache->shards);
    memset(cache, 0, sizeof(*cache));
}

static uint64_t timestamp_distance(uint64_t a, uint64_t b) { return a > b ? a - b : b - a; }

static struct dedup_rejected_entry *rejected_find_best(
        struct dedup_cache *cache, struct dedup_shard *s,
        const struct packet_fingerprint *fp, const uint8_t agent_uuid[16],
        uint64_t adjusted_timestamp_ns) {
    size_t bucket = fingerprint_hash(fp->bytes) % s->rejected_bucket_count;
    struct dedup_rejected_entry *best = NULL;
    uint64_t best_distance = UINT64_MAX;
    for (struct dedup_rejected_entry *it = s->rejected_buckets[bucket]; it;
         it = it->hash_next) {
        if (memcmp(it->fingerprint, fp->bytes, 16) ||
            memcmp(it->agent_uuid, agent_uuid, 16) == 0 ||
            it->ip_protocol != fp->ip_protocol ||
            it->payload_length != fp->payload_length ||
            memcmp(it->payload_hash, fp->payload_hash, 16))
            continue;
        uint64_t distance = timestamp_distance(it->adjusted_timestamp_ns,
                                               adjusted_timestamp_ns);
        if (distance > cache->tolerance_ns) continue;
        if (!best || distance < best_distance ||
            (distance == best_distance &&
             it->receiver_monotonic_insert_ns < best->receiver_monotonic_insert_ns)) {
            best = it;
            best_distance = distance;
        }
    }
    return best;
}

static void rejected_add(struct dedup_cache *cache, struct dedup_shard *s,
                         const struct packet_fingerprint *fp,
                         const uint8_t agent_uuid[16], uint64_t adjusted_timestamp_ns,
                         uint64_t now) {
    if (!s->rejected_capacity) return;
    if (s->rejected_count >= s->rejected_capacity && s->rejected_oldest)
        rejected_remove_known(cache, s, s->rejected_oldest);
    struct dedup_rejected_entry *entry = calloc(1, sizeof(*entry));
    if (!entry) return;
    memcpy(entry->fingerprint, fp->bytes, 16);
    memcpy(entry->payload_hash, fp->payload_hash, 16);
    memcpy(entry->agent_uuid, agent_uuid, 16);
    entry->adjusted_timestamp_ns = adjusted_timestamp_ns;
    entry->receiver_monotonic_insert_ns = now;
    entry->payload_length = fp->payload_length;
    entry->ip_protocol = fp->ip_protocol;
    size_t bucket = fingerprint_hash(fp->bytes) % s->rejected_bucket_count;
    entry->hash_next = s->rejected_buckets[bucket];
    s->rejected_buckets[bucket] = entry;
    entry->older = s->rejected_newest;
    if (s->rejected_newest) s->rejected_newest->newer = entry;
    else s->rejected_oldest = entry;
    s->rejected_newest = entry;
    s->rejected_count++;
    __atomic_add_fetch(&cache->rejected_entries, 1, __ATOMIC_RELAXED);
}

static enum dedup_submit_result dedup_cache_process(struct dedup_cache *cache,
        const struct packet_fingerprint *fp, const uint8_t agent_uuid[16],
        uint64_t capture_timestamp_ns, bool timestamp_valid, uint64_t adjusted_timestamp_ns,
        uint64_t now, dedup_submit_fn submit, void *submit_context) {
    if (!cache->shards || !timestamp_valid) {
        return submit && submit(submit_context) != 0 ?
            DEDUP_SUBMIT_REJECTED : DEDUP_SUBMIT_ACCEPTED;
    }
    uint64_t hash = fingerprint_hash(fp->bytes);
    struct dedup_shard *s = &cache->shards[hash % cache->shard_count];
    pthread_mutex_lock(&s->mutex);
    expire_old(cache, s, now);
    rejected_expire_old(cache, s, now);
    size_t bucket = hash % s->bucket_count;
    struct dedup_entry *best = NULL;
    uint64_t best_distance = UINT64_MAX;
    for (struct dedup_entry *it = s->buckets[bucket]; it; it = it->hash_next) {
        if (memcmp(it->fingerprint, fp->bytes, 16) || memcmp(it->agent_uuid, agent_uuid, 16) == 0 ||
            it->ip_protocol != fp->ip_protocol || it->payload_length != fp->payload_length ||
            memcmp(it->payload_hash, fp->payload_hash, 16) || !it->timestamp_valid)
            continue;
        uint64_t distance = timestamp_distance(it->adjusted_timestamp_ns, adjusted_timestamp_ns);
        if (distance > cache->tolerance_ns) continue;
        if (!best || distance < best_distance ||
            (distance == best_distance && it->receiver_monotonic_insert_ns < best->receiver_monotonic_insert_ns)) {
            best = it; best_distance = distance;
        }
    }
    if (best) {
        remove_known(cache, s, best);
        pthread_mutex_unlock(&s->mutex);
        return DEDUP_SUBMIT_CROSS_AGENT_DUPLICATE;
    }
    /* The entry must never suppress another copy unless this copy reached the output queue. */
    struct dedup_rejected_entry *rejected = submit ?
        rejected_find_best(cache, s, fp, agent_uuid, adjusted_timestamp_ns) : NULL;
    if (submit && submit(submit_context) != 0) {
        if (rejected) {
            rejected_remove_known(cache, s, rejected);
            __atomic_add_fetch(&cache->rejected_observation_pairs, 1, __ATOMIC_RELAXED);
        } else {
            rejected_add(cache, s, fp, agent_uuid, adjusted_timestamp_ns, now);
        }
        pthread_mutex_unlock(&s->mutex);
        return DEDUP_SUBMIT_REJECTED;
    }
    if (rejected) {
        rejected_remove_known(cache, s, rejected);
        __atomic_add_fetch(&cache->peer_rescued_after_reject, 1, __ATOMIC_RELAXED);
    }
    if (s->count >= s->capacity && s->oldest) {
        remove_known(cache, s, s->oldest);
        __atomic_add_fetch(&cache->evictions, 1, __ATOMIC_RELAXED);
    }
    struct dedup_entry *entry = calloc(1, sizeof(*entry));
    if (!entry) { pthread_mutex_unlock(&s->mutex); return DEDUP_SUBMIT_ACCEPTED; }
    memcpy(entry->fingerprint, fp->bytes, 16);
    memcpy(entry->payload_hash, fp->payload_hash, 16);
    memcpy(entry->agent_uuid, agent_uuid, 16);
    entry->capture_timestamp_ns = capture_timestamp_ns;
    entry->adjusted_timestamp_ns = adjusted_timestamp_ns;
    entry->receiver_monotonic_insert_ns = now;
    entry->payload_length = fp->payload_length;
    entry->ip_protocol = fp->ip_protocol;
    entry->timestamp_valid = timestamp_valid;
    entry->hash_next = s->buckets[bucket]; s->buckets[bucket] = entry;
    entry->older = s->newest;
    if (s->newest) s->newest->newer = entry; else s->oldest = entry;
    s->newest = entry; s->count++;
    __atomic_add_fetch(&cache->entries, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&s->mutex);
    return DEDUP_SUBMIT_ACCEPTED;
}

enum dedup_result dedup_cache_check_and_add(struct dedup_cache *cache,
        const struct packet_fingerprint *fp, const uint8_t agent_uuid[16],
        uint64_t capture_timestamp_ns, bool timestamp_valid, uint64_t adjusted_timestamp_ns,
        uint64_t now) {
    return dedup_cache_process(cache, fp, agent_uuid, capture_timestamp_ns,
            timestamp_valid, adjusted_timestamp_ns, now, NULL, NULL) ==
            DEDUP_SUBMIT_CROSS_AGENT_DUPLICATE ?
            DEDUP_CROSS_AGENT_DUPLICATE : DEDUP_PASS;
}

enum dedup_submit_result dedup_cache_submit(struct dedup_cache *cache,
        const struct packet_fingerprint *fp, const uint8_t agent_uuid[16],
        uint64_t capture_timestamp_ns, bool timestamp_valid, uint64_t adjusted_timestamp_ns,
        uint64_t now, dedup_submit_fn submit, void *submit_context) {
    return dedup_cache_process(cache, fp, agent_uuid, capture_timestamp_ns,
            timestamp_valid, adjusted_timestamp_ns, now, submit, submit_context);
}

bool dedup_adjust_timestamp(const struct receiver_config *cfg, const uint8_t uuid[16],
                            uint64_t timestamp, uint64_t *adjusted) {
    if (!timestamp) return false;
    int64_t offset = 0;
    for (size_t i = 0; i < cfg->clock_offset_count; i++)
        if (memcmp(cfg->clock_offsets[i].agent_uuid, uuid, 16) == 0) {
            offset = cfg->clock_offsets[i].offset_ns; break;
        }
    if (offset >= 0) {
        if (timestamp > UINT64_MAX - (uint64_t)offset) return false;
        *adjusted = timestamp + (uint64_t)offset;
    } else {
        uint64_t magnitude = (uint64_t)(-(offset + 1)) + 1;
        if (timestamp <= magnitude) return false;
        *adjusted = timestamp - magnitude;
    }
    return *adjusted != 0;
}

static size_t transport_hash(const uint8_t uuid[16], uint64_t epoch, size_t capacity) {
    return (size_t)(fingerprint_hash(uuid) ^ epoch ^ (epoch >> 32)) % capacity;
}

int transport_registry_init(struct transport_registry *r, size_t capacity) {
    memset(r, 0, sizeof(*r)); r->capacity = capacity;
    r->entries = calloc(capacity, sizeof(*r->entries));
    if (!r->entries) return -1;
    if (pthread_mutex_init(&r->mutex, NULL) != 0) { free(r->entries); r->entries = NULL; return -1; }
    return 0;
}

void transport_registry_destroy(struct transport_registry *r) {
    if (!r || !r->entries) return;
    pthread_mutex_destroy(&r->mutex); free(r->entries); memset(r, 0, sizeof(*r));
}

static struct transport_entry *transport_find(struct transport_registry *r,
        const uint8_t uuid[16], uint64_t epoch, bool create, uint64_t now) {
    size_t start = transport_hash(uuid, epoch, r->capacity);
    struct transport_entry *free_slot = NULL, *oldest_inactive = NULL, *oldest = NULL;
    for (size_t n = 0; n < r->capacity; n++) {
        struct transport_entry *e = &r->entries[(start + n) % r->capacity];
        if (e->used && e->epoch == epoch && memcmp(e->agent_uuid, uuid, 16) == 0) return e;
        if (!e->used && !free_slot) free_slot = e;
        if (e->used && (!oldest || e->last_seen_ns < oldest->last_seen_ns)) oldest = e;
        if (e->used && !e->active && (!oldest_inactive || e->last_seen_ns < oldest_inactive->last_seen_ns)) oldest_inactive = e;
    }
    if (!create) return NULL;
    struct transport_entry *e = free_slot ? free_slot : (oldest_inactive ? oldest_inactive : oldest);
    memset(e, 0, sizeof(*e)); e->used = true; e->epoch = epoch; e->last_seen_ns = now;
    memcpy(e->agent_uuid, uuid, 16); return e;
}

void transport_registry_register(struct transport_registry *r, const uint8_t uuid[16],
                                 uint64_t epoch, uint64_t now) {
    pthread_mutex_lock(&r->mutex);
    struct transport_entry *e = transport_find(r, uuid, epoch, true, now);
    e->active++; e->last_seen_ns = now;
    pthread_mutex_unlock(&r->mutex);
}

void transport_registry_unregister(struct transport_registry *r, const uint8_t uuid[16], uint64_t epoch) {
    pthread_mutex_lock(&r->mutex);
    struct transport_entry *e = transport_find(r, uuid, epoch, false, 0);
    if (e && e->active) e->active--;
    pthread_mutex_unlock(&r->mutex);
}

enum transport_result transport_registry_check(struct transport_registry *r,
        const uint8_t uuid[16], uint64_t epoch, uint64_t sequence, uint64_t now, uint64_t *gap) {
    *gap = 0;
    pthread_mutex_lock(&r->mutex);
    struct transport_entry *e = transport_find(r, uuid, epoch, true, now);
    enum transport_result result = TRANSPORT_ACCEPT;
    if (e->sequence_seen && sequence == e->last_sequence) result = TRANSPORT_DUPLICATE;
    else if (e->sequence_seen && sequence < e->last_sequence) result = TRANSPORT_OUT_OF_ORDER;
    else {
        if (e->sequence_seen && sequence > e->last_sequence + 1) *gap = sequence - e->last_sequence - 1;
        e->last_sequence = sequence;
        e->sequence_seen = true;
    }
    e->last_seen_ns = now;
    pthread_mutex_unlock(&r->mutex);
    return result;
}
