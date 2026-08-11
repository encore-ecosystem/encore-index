/*
 * Common EHIR node-handle ABI.
 *
 * This file intentionally has no dependency on the rest of core's hosted
 * runtime. It is both included by runtime.c and compiled on its own during a
 * compiler/runtime ABI bootstrap.
 *
 * A public handle is exactly the payload address. The machine word directly
 * before that address points at the node header, so <S>, <H>, and placement-
 * erased handles have the same representation. Heap nodes keep header and
 * payload in one allocation; stack nodes keep only their payload in the
 * caller's frame and use a small side header until logical death.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>

enum {
    ENCORE_NODE_PREFIX_BYTES = 128,
    ENCORE_NODE_HEAP = 1,
    ENCORE_NODE_STACK = 2,
    ENCORE_NODE_ALIVE = 1,
    ENCORE_NODE_RELEASED = 2,
    ENCORE_NODE_FINALIZING = 3
};

typedef struct encore_node_header {
    _Atomic size_t refs;
    void *allocation_base;
    _Atomic size_t incoming_edges;
    void **edges;
    size_t edge_count;
    size_t edge_capacity;
    void **incoming_owners;
    size_t incoming_owner_count;
    size_t incoming_owner_capacity;
    uint32_t placement;
    _Atomic uint32_t state;
} encore_node_header;

typedef struct encore_node_header_vec {
    encore_node_header **items;
    size_t len;
    size_t capacity;
} encore_node_header_vec;

typedef struct encore_node_index_map {
    encore_node_header **keys;
    size_t *values;
    size_t len;
    size_t capacity;
} encore_node_index_map;

static atomic_flag g_encore_node_graph_lock = ATOMIC_FLAG_INIT;
static _Atomic size_t g_encore_node_live_count = 0;
static _Atomic size_t g_encore_node_ern_scan_count = 0;
static _Thread_local encore_node_header_vec g_encore_node_finalizers = {0};

static void encore_node_graph_lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_encore_node_graph_lock,
                                             memory_order_acquire)) {
    }
}

static void encore_node_graph_unlock(void) {
    atomic_flag_clear_explicit(&g_encore_node_graph_lock, memory_order_release);
}

_Static_assert(sizeof(encore_node_header) + sizeof(encore_node_header *) <=
                   ENCORE_NODE_PREFIX_BYTES,
               "EHIR node header must fit before its payload");

static encore_node_header *encore_node_header_from_payload(void *payload) {
    if (payload == NULL) return NULL;
    return ((encore_node_header **)payload)[-1];
}

static void encore_node_initialize_header(encore_node_header *header,
                                          void *allocation_base,
                                          size_t payload_size,
                                          uint32_t placement) {
    atomic_init(&header->refs, 1);
    header->allocation_base = allocation_base;
    (void)payload_size;
    atomic_init(&header->incoming_edges, 0);
    header->edges = NULL;
    header->edge_count = 0;
    header->edge_capacity = 0;
    header->incoming_owners = NULL;
    header->incoming_owner_count = 0;
    header->incoming_owner_capacity = 0;
    header->placement = placement;
    atomic_init(&header->state, ENCORE_NODE_ALIVE);
}

static void encore_node_reserve_edges(encore_node_header *header,
                                      size_t required) {
    if (required <= header->edge_capacity) return;
    size_t capacity = header->edge_capacity == 0 ? 4 : header->edge_capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) abort();
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(void *)) abort();
    void **next = realloc(header->edges, capacity * sizeof(void *));
    if (next == NULL) abort();
    header->edges = next;
    header->edge_capacity = capacity;
}

static void encore_node_reserve_incoming_owners(encore_node_header *header,
                                                size_t required) {
    if (required <= header->incoming_owner_capacity) return;
    size_t capacity = header->incoming_owner_capacity == 0
        ? 4
        : header->incoming_owner_capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) abort();
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(void *)) abort();
    void **next = realloc(header->incoming_owners,
                          capacity * sizeof(void *));
    if (next == NULL) abort();
    header->incoming_owners = next;
    header->incoming_owner_capacity = capacity;
}

static void encore_node_add_incoming_owner(encore_node_header *target,
                                           encore_node_header *owner) {
    encore_node_reserve_incoming_owners(
        target, target->incoming_owner_count + 1);
    target->incoming_owners[target->incoming_owner_count++] = owner;
}

static void encore_node_remove_incoming_owner(encore_node_header *target,
                                              encore_node_header *owner) {
    size_t index = SIZE_MAX;
    for (size_t cursor = 0; cursor < target->incoming_owner_count; ++cursor) {
        if (target->incoming_owners[cursor] == owner) {
            index = cursor;
            break;
        }
    }
    if (index == SIZE_MAX) abort();
    target->incoming_owners[index] =
        target->incoming_owners[target->incoming_owner_count - 1];
    target->incoming_owner_count -= 1;
}

static size_t encore_node_find_edge(encore_node_header *header, void *target) {
    for (size_t index = 0; index < header->edge_count; ++index) {
        if (header->edges[index] == target) return index;
    }
    return SIZE_MAX;
}

void encore_node_add_edge(void *owner_payload, void *target_payload) {
    if (owner_payload == NULL || target_payload == NULL) return;
    encore_node_header *owner = encore_node_header_from_payload(owner_payload);
    encore_node_header *target = encore_node_header_from_payload(target_payload);
    if (owner == NULL || target == NULL) abort();
    encore_node_graph_lock();
    if (owner->state != ENCORE_NODE_ALIVE ||
        target->state != ENCORE_NODE_ALIVE) {
        encore_node_graph_unlock();
        abort();
    }
    encore_node_reserve_edges(owner, owner->edge_count + 1);
    owner->edges[owner->edge_count++] = target_payload;
    encore_node_add_incoming_owner(target, owner);
    if (target->incoming_edges == SIZE_MAX) {
        encore_node_graph_unlock();
        abort();
    }
    target->incoming_edges += 1;
    encore_node_graph_unlock();
}

void encore_node_remove_edge(void *owner_payload, void *target_payload) {
    if (owner_payload == NULL || target_payload == NULL) return;
    encore_node_header *owner = encore_node_header_from_payload(owner_payload);
    if (owner == NULL) abort();
    encore_node_graph_lock();
    if (owner->state != ENCORE_NODE_ALIVE &&
        owner->state != ENCORE_NODE_FINALIZING) {
        encore_node_graph_unlock();
        abort();
    }
    size_t index = encore_node_find_edge(owner, target_payload);
    if (index == SIZE_MAX) {
        encore_node_graph_unlock();
        abort();
    }
    encore_node_header *target = encore_node_header_from_payload(target_payload);
    if (target == NULL || target->incoming_edges == 0) {
        encore_node_graph_unlock();
        abort();
    }
    target->incoming_edges -= 1;
    encore_node_remove_incoming_owner(target, owner);
    owner->edges[index] = owner->edges[owner->edge_count - 1];
    owner->edge_count -= 1;
    encore_node_graph_unlock();
}

void encore_node_replace_edge(void *owner_payload,
                              void *old_target_payload,
                              void *new_target_payload) {
    if (old_target_payload == new_target_payload) return;
    if (owner_payload == NULL) abort();
    encore_node_header *owner = encore_node_header_from_payload(owner_payload);
    encore_node_header *new_target =
        encore_node_header_from_payload(new_target_payload);
    if (owner == NULL) abort();
    encore_node_graph_lock();
    if (owner->state != ENCORE_NODE_ALIVE ||
        (new_target != NULL && new_target->state != ENCORE_NODE_ALIVE)) {
        encore_node_graph_unlock();
        abort();
    }
    size_t old_index = old_target_payload == NULL
        ? SIZE_MAX
        : encore_node_find_edge(owner, old_target_payload);
    if (old_target_payload != NULL && old_index == SIZE_MAX) {
        encore_node_graph_unlock();
        abort();
    }
    if (new_target_payload != NULL && old_index == SIZE_MAX) {
        encore_node_reserve_edges(owner, owner->edge_count + 1);
        owner->edges[owner->edge_count++] = new_target_payload;
        encore_node_add_incoming_owner(new_target, owner);
        if (new_target->incoming_edges == SIZE_MAX) {
            encore_node_graph_unlock();
            abort();
        }
        new_target->incoming_edges += 1;
    } else if (new_target_payload != NULL) {
        encore_node_header *old_target =
            encore_node_header_from_payload(old_target_payload);
        if (old_target == NULL || old_target->incoming_edges == 0 ||
            new_target->incoming_edges == SIZE_MAX) {
            encore_node_graph_unlock();
            abort();
        }
        old_target->incoming_edges -= 1;
        new_target->incoming_edges += 1;
        encore_node_remove_incoming_owner(old_target, owner);
        encore_node_add_incoming_owner(new_target, owner);
        owner->edges[old_index] = new_target_payload;
    } else if (old_index != SIZE_MAX) {
        encore_node_header *old_target =
            encore_node_header_from_payload(old_target_payload);
        if (old_target == NULL || old_target->incoming_edges == 0) {
            encore_node_graph_unlock();
            abort();
        }
        old_target->incoming_edges -= 1;
        encore_node_remove_incoming_owner(old_target, owner);
        owner->edges[old_index] = owner->edges[owner->edge_count - 1];
        owner->edge_count -= 1;
    }
    encore_node_graph_unlock();
}

static void encore_node_header_vec_push(encore_node_header_vec *vec,
                                        encore_node_header *header) {
    if (vec->len == vec->capacity) {
        size_t capacity = vec->capacity == 0 ? 8 : vec->capacity * 2;
        if (capacity < vec->capacity ||
            capacity > SIZE_MAX / sizeof(encore_node_header *)) {
            abort();
        }
        encore_node_header **next =
            realloc(vec->items, capacity * sizeof(encore_node_header *));
        if (next == NULL) abort();
        vec->items = next;
        vec->capacity = capacity;
    }
    vec->items[vec->len++] = header;
}

static void encore_node_detach_finalizer_edge(encore_node_header *target,
                                               void *target_payload) {
    if (g_encore_node_finalizers.len == 0) return;
    encore_node_header *owner = g_encore_node_finalizers.items[
        g_encore_node_finalizers.len - 1];
    size_t edge = encore_node_find_edge(owner, target_payload);
    if (edge == SIZE_MAX) return;
    if (target->incoming_edges == 0) abort();
    target->incoming_edges -= 1;
    encore_node_remove_incoming_owner(target, owner);
    owner->edges[edge] = owner->edges[owner->edge_count - 1];
    owner->edge_count -= 1;
}

static size_t encore_node_index_hash(encore_node_header *header) {
    uintptr_t bits = (uintptr_t)header;
    bits ^= bits >> 33;
    bits *= UINT64_C(0xff51afd7ed558ccd);
    bits ^= bits >> 33;
    return (size_t)bits;
}

static void encore_node_index_map_rehash(encore_node_index_map *map,
                                         size_t capacity) {
    encore_node_header **old_keys = map->keys;
    size_t *old_values = map->values;
    size_t old_capacity = map->capacity;
    map->keys = calloc(capacity, sizeof(encore_node_header *));
    map->values = malloc(capacity * sizeof(size_t));
    if (map->keys == NULL || map->values == NULL) abort();
    map->capacity = capacity;
    map->len = 0;
    for (size_t index = 0; index < old_capacity; ++index) {
        encore_node_header *key = old_keys[index];
        if (key == NULL) continue;
        size_t slot = encore_node_index_hash(key) & (capacity - 1);
        while (map->keys[slot] != NULL) slot = (slot + 1) & (capacity - 1);
        map->keys[slot] = key;
        map->values[slot] = old_values[index];
        map->len += 1;
    }
    free(old_values);
    free(old_keys);
}

static void encore_node_index_map_insert(encore_node_index_map *map,
                                         encore_node_header *header,
                                         size_t value) {
    if (map->capacity == 0) encore_node_index_map_rehash(map, 16);
    if ((map->len + 1) * 10 >= map->capacity * 7) {
        if (map->capacity > SIZE_MAX / 2) abort();
        encore_node_index_map_rehash(map, map->capacity * 2);
    }
    size_t slot = encore_node_index_hash(header) & (map->capacity - 1);
    while (map->keys[slot] != NULL) {
        if (map->keys[slot] == header) return;
        slot = (slot + 1) & (map->capacity - 1);
    }
    map->keys[slot] = header;
    map->values[slot] = value;
    map->len += 1;
}

static size_t encore_node_index_map_find(const encore_node_index_map *map,
                                         encore_node_header *header) {
    if (header == NULL || map->capacity == 0) return SIZE_MAX;
    size_t slot = encore_node_index_hash(header) & (map->capacity - 1);
    while (map->keys[slot] != NULL) {
        if (map->keys[slot] == header) return map->values[slot];
        slot = (slot + 1) & (map->capacity - 1);
    }
    return SIZE_MAX;
}

static void encore_node_index_map_free(encore_node_index_map *map) {
    free(map->values);
    free(map->keys);
}

/*
 * A transient root borrowed from a field commonly leaves `refs == incoming`
 * when it is released. Trial-deleting the entire outgoing object graph in
 * that case is unnecessary when an incoming owner is itself reachable from
 * an external root. Walk the usually shallow owner chain first. This is an
 * exact fast rejection: failure to find a root falls back to the full ERN
 * algorithm, so cycles and rootless components keep their existing behavior.
 */
static bool encore_node_has_external_ancestor(encore_node_header *root) {
    encore_node_header_vec pending = {0};
    encore_node_index_map seen = {0};
    encore_node_header_vec_push(&pending, root);
    encore_node_index_map_insert(&seen, root, 0);
    bool found = false;
    for (size_t cursor = 0; cursor < pending.len && !found; ++cursor) {
        encore_node_header *node = pending.items[cursor];
        if (node->state != ENCORE_NODE_ALIVE) continue;
        size_t refs = atomic_load_explicit(&node->refs,
                                           memory_order_relaxed);
        size_t incoming = atomic_load_explicit(&node->incoming_edges,
                                               memory_order_relaxed);
        if (node != root && refs > incoming) {
            found = true;
            break;
        }
        for (size_t index = 0; index < node->incoming_owner_count; ++index) {
            encore_node_header *owner =
                (encore_node_header *)node->incoming_owners[index];
            if (owner == NULL || owner->state != ENCORE_NODE_ALIVE) continue;
            if (encore_node_index_map_find(&seen, owner) == SIZE_MAX) {
                encore_node_index_map_insert(&seen, owner, pending.len);
                encore_node_header_vec_push(&pending, owner);
            }
        }
    }
    free(pending.items);
    encore_node_index_map_free(&seen);
    return found;
}

/*
 * Computes ERN(root) by trial deletion while the graph lock is held.
 * refs - internal_indegree is the number of incoming roots/edges from outside
 * the reachable candidate. Every such survivor keeps all of its descendants
 * alive. The unmarked remainder is exclusively reachable from root.
 */
static bool encore_node_mark_ern(encore_node_header *root) {
    atomic_fetch_add_explicit(&g_encore_node_ern_scan_count, 1,
                              memory_order_relaxed);
    encore_node_header_vec reachable = {0};
    encore_node_index_map indices = {0};
    encore_node_header_vec_push(&reachable, root);
    encore_node_index_map_insert(&indices, root, 0);
    for (size_t cursor = 0; cursor < reachable.len; ++cursor) {
        encore_node_header *source = reachable.items[cursor];
        for (size_t edge = 0; edge < source->edge_count; ++edge) {
            encore_node_header *target =
                encore_node_header_from_payload(source->edges[edge]);
            if (target == NULL || target->state != ENCORE_NODE_ALIVE) continue;
            if (encore_node_index_map_find(&indices, target) == SIZE_MAX) {
                encore_node_index_map_insert(&indices, target, reachable.len);
                encore_node_header_vec_push(&reachable, target);
            }
        }
    }

    size_t *internal_indegree = calloc(reachable.len, sizeof(size_t));
    bool *survives = calloc(reachable.len, sizeof(bool));
    size_t *queue = malloc(reachable.len * sizeof(size_t));
    if (internal_indegree == NULL || survives == NULL || queue == NULL) abort();

    for (size_t source_index = 0; source_index < reachable.len;
         ++source_index) {
        encore_node_header *source = reachable.items[source_index];
        for (size_t edge = 0; edge < source->edge_count; ++edge) {
            encore_node_header *target =
                encore_node_header_from_payload(source->edges[edge]);
            size_t target_index = encore_node_index_map_find(&indices, target);
            if (target_index != SIZE_MAX) internal_indegree[target_index] += 1;
        }
    }

    size_t queue_len = 0;
    for (size_t index = 0; index < reachable.len; ++index) {
        size_t refs = atomic_load_explicit(&reachable.items[index]->refs,
                                           memory_order_relaxed);
        if (refs < internal_indegree[index]) abort();
        if (refs - internal_indegree[index] > 0) {
            survives[index] = true;
            queue[queue_len++] = index;
        }
    }

    for (size_t cursor = 0; cursor < queue_len; ++cursor) {
        encore_node_header *source = reachable.items[queue[cursor]];
        for (size_t edge = 0; edge < source->edge_count; ++edge) {
            encore_node_header *target =
                encore_node_header_from_payload(source->edges[edge]);
            size_t target_index = encore_node_index_map_find(&indices, target);
            if (target_index != SIZE_MAX && !survives[target_index]) {
                survives[target_index] = true;
                queue[queue_len++] = target_index;
            }
        }
    }

    bool root_released = false;
    for (size_t index = 0; index < reachable.len; ++index) {
        if (!survives[index]) {
            reachable.items[index]->state = ENCORE_NODE_RELEASED;
            if (reachable.items[index] == root) root_released = true;
        }
    }

    free(queue);
    free(survives);
    free(internal_indegree);
    free(reachable.items);
    encore_node_index_map_free(&indices);
    return root_released;
}

void *encore_node_alloc_heap(size_t payload_size) {
    if (payload_size == 0) payload_size = 1;
    if (payload_size > SIZE_MAX - ENCORE_NODE_PREFIX_BYTES) return NULL;
    unsigned char *allocation = malloc(ENCORE_NODE_PREFIX_BYTES + payload_size);
    if (allocation == NULL) return NULL;
    encore_node_header *header = (encore_node_header *)allocation;
    void *payload = allocation + ENCORE_NODE_PREFIX_BYTES;
    encore_node_initialize_header(header, allocation, payload_size,
                                  ENCORE_NODE_HEAP);
    ((encore_node_header **)payload)[-1] = header;
    atomic_fetch_add_explicit(&g_encore_node_live_count, 1,
                              memory_order_relaxed);
    return payload;
}

void encore_node_register_stack(void *payload, size_t payload_size) {
    if (payload == NULL) abort();
    encore_node_header *header = malloc(sizeof(encore_node_header));
    if (header == NULL) abort();
    encore_node_initialize_header(header, NULL, payload_size,
                                  ENCORE_NODE_STACK);
    ((encore_node_header **)payload)[-1] = header;
    atomic_fetch_add_explicit(&g_encore_node_live_count, 1,
                              memory_order_relaxed);
}

void encore_node_retain(void *payload) {
    encore_node_header *header = encore_node_header_from_payload(payload);
    if (header == NULL) return;
    /*
     * A retain is made through an already-live root or edge, so that owner
     * prevents concurrent ERN reclamation.  It only needs the atomic root
     * count; serializing every read-only Vec copy on the graph lock made
     * parallel compiler lowering effectively single-threaded.
     */
    if (atomic_load_explicit(&header->state, memory_order_acquire) !=
        ENCORE_NODE_ALIVE) abort();
    size_t previous = atomic_load_explicit(&header->refs,
                                           memory_order_relaxed);
    for (;;) {
        if (previous == 0 || previous == SIZE_MAX) abort();
        if (atomic_compare_exchange_weak_explicit(
                &header->refs, &previous, previous + 1,
                memory_order_relaxed, memory_order_relaxed)) break;
        if (atomic_load_explicit(&header->state, memory_order_acquire) !=
            ENCORE_NODE_ALIVE) abort();
    }
    if (atomic_load_explicit(&header->state, memory_order_acquire) !=
        ENCORE_NODE_ALIVE) {
        atomic_fetch_sub_explicit(&header->refs, 1, memory_order_relaxed);
        abort();
    }
}

bool encore_node_release(void *payload) {
    encore_node_header *header = encore_node_header_from_payload(payload);
    if (header == NULL) return false;
    /*
     * Most releases cannot possibly expose an ERN boundary.  Remove those
     * roots lock-free.  Edge changes remain serialized and publish their
     * incoming count atomically; the slow path rechecks everything while
     * holding the graph lock.
     */
    if (g_encore_node_finalizers.len == 0 &&
        atomic_load_explicit(&header->state, memory_order_acquire) ==
            ENCORE_NODE_ALIVE) {
        size_t refs = atomic_load_explicit(&header->refs,
                                           memory_order_relaxed);
        while (refs > 0) {
            size_t incoming = atomic_load_explicit(&header->incoming_edges,
                                                   memory_order_acquire);
            if (refs == 1 && incoming == 0) {
                if (atomic_compare_exchange_weak_explicit(
                        &header->refs, &refs, 0,
                        memory_order_release, memory_order_relaxed)) {
                    uint32_t expected = ENCORE_NODE_ALIVE;
                    if (!atomic_compare_exchange_strong_explicit(
                            &header->state, &expected,
                            ENCORE_NODE_FINALIZING,
                            memory_order_acq_rel, memory_order_acquire)) {
                        abort();
                    }
                    encore_node_header_vec_push(
                        &g_encore_node_finalizers, header);
                    return true;
                }
                continue;
            }
            if (refs <= 1 || incoming >= refs - 1) break;
            if (atomic_compare_exchange_weak_explicit(
                    &header->refs, &refs, refs - 1,
                    memory_order_release, memory_order_relaxed)) {
                return false;
            }
            if (atomic_load_explicit(&header->state, memory_order_acquire) !=
                ENCORE_NODE_ALIVE) break;
        }
    }
    encore_node_graph_lock();
    encore_node_detach_finalizer_edge(header, payload);
    if (header->state == ENCORE_NODE_RELEASED) {
        header->state = ENCORE_NODE_FINALIZING;
        encore_node_header_vec_push(&g_encore_node_finalizers, header);
        encore_node_graph_unlock();
        return true;
    }
    if (header->state != ENCORE_NODE_ALIVE) {
        encore_node_graph_unlock();
        return false;
    }
    size_t refs = atomic_load_explicit(&header->refs, memory_order_relaxed);
    if (refs == 0) {
        encore_node_graph_unlock();
        abort();
    }
    refs = atomic_fetch_sub_explicit(&header->refs, 1,
                                     memory_order_release) - 1;
    if (refs < header->incoming_edges) {
        encore_node_graph_unlock();
        abort();
    }
    /*
     * At least one reference not represented by a graph edge still reaches
     * this node. It therefore survives, as do all of its descendants, so
     * ERN(root) cannot contain the root and trial deletion is unnecessary.
     */
    if (refs > header->incoming_edges) {
        encore_node_graph_unlock();
        return false;
    }
    if (refs == 0 && header->incoming_edges == 0) {
        header->state = ENCORE_NODE_FINALIZING;
        encore_node_header_vec_push(&g_encore_node_finalizers, header);
        encore_node_graph_unlock();
        return true;
    }
    if (encore_node_has_external_ancestor(header)) {
        encore_node_graph_unlock();
        return false;
    }
    bool should_finalize = encore_node_mark_ern(header);
    if (should_finalize) {
        header->state = ENCORE_NODE_FINALIZING;
        encore_node_header_vec_push(&g_encore_node_finalizers, header);
    }
    encore_node_graph_unlock();
    return should_finalize;
}

void encore_node_free_released(void *payload) {
    encore_node_header *header = encore_node_header_from_payload(payload);
    if (header == NULL) return;
    bool can_free = header->state == ENCORE_NODE_FINALIZING;
    uint32_t placement = header->placement;
    void *allocation_base = header->allocation_base;
    void **edges = header->edges;
    void **incoming_owners = header->incoming_owners;
    if (can_free) {
        if (g_encore_node_finalizers.len == 0 ||
            g_encore_node_finalizers.items[
                g_encore_node_finalizers.len - 1] != header) {
            abort();
        }
        g_encore_node_finalizers.len -= 1;
        size_t live = atomic_fetch_sub_explicit(
            &g_encore_node_live_count, 1, memory_order_relaxed);
        if (live == 0) {
            abort();
        }
    }
    if (placement == ENCORE_NODE_STACK) {
        ((encore_node_header **)payload)[-1] = NULL;
    }
    if (!can_free) abort();
    free(edges);
    free(incoming_owners);
    if (placement == ENCORE_NODE_HEAP) free(allocation_base);
    else free(header);
}

size_t encore_node_debug_ref_count(void *payload) {
    encore_node_header *header = encore_node_header_from_payload(payload);
    if (header == NULL) return 0;
    return atomic_load_explicit(&header->refs, memory_order_acquire);
}

bool encore_node_debug_is_stack(void *payload) {
    encore_node_header *header = encore_node_header_from_payload(payload);
    return header != NULL && header->placement == ENCORE_NODE_STACK;
}

size_t encore_node_debug_edge_count(void *payload) {
    encore_node_header *header = encore_node_header_from_payload(payload);
    if (header == NULL) return 0;
    return header->edge_count;
}

size_t encore_node_debug_live_count(void) {
    return atomic_load_explicit(&g_encore_node_live_count,
                                memory_order_acquire);
}

size_t encore_node_debug_ern_scan_count(void) {
    return atomic_load_explicit(&g_encore_node_ern_scan_count,
                                memory_order_acquire);
}
