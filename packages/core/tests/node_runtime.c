#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void *encore_node_alloc_heap(size_t payload_size);
void encore_node_register_stack(void *payload, size_t payload_size);
void encore_node_retain(void *payload);
bool encore_node_release(void *payload);
void encore_node_free_released(void *payload);
void encore_node_add_edge(void *owner_payload, void *target_payload);
void encore_node_replace_edge(void *owner_payload, void *old_target_payload,
                              void *new_target_payload);
size_t encore_node_debug_ref_count(void *payload);
bool encore_node_debug_is_stack(void *payload);
size_t encore_node_debug_edge_count(void *payload);
size_t encore_node_debug_live_count(void);
size_t encore_node_debug_ern_scan_count(void);

typedef struct stack_u64_node {
    uint64_t node_prefix[8];
    uint64_t payload;
} stack_u64_node;

static void connect(void *owner, void *target) {
    encore_node_retain(target);
    encore_node_add_edge(owner, target);
}

int main(void) {
    assert(encore_node_debug_live_count() == 0);
    uint64_t *heap = encore_node_alloc_heap(sizeof(uint64_t));
    assert(heap != NULL);
    assert(!encore_node_debug_is_stack(heap));
    assert(encore_node_debug_ref_count(heap) == 1);
    *heap = UINT64_C(42);
    encore_node_retain(heap);
    assert(encore_node_debug_ref_count(heap) == 2);
    assert(!encore_node_release(heap));
    assert(encore_node_debug_ref_count(heap) == 1);
    assert(encore_node_release(heap));
    encore_node_free_released(heap);

    stack_u64_node storage = {{0}, 0};
    uint64_t *stack = &storage.payload;
    encore_node_register_stack(stack, sizeof(uint64_t));
    assert(encore_node_debug_is_stack(stack));
    assert(encore_node_debug_ref_count(stack) == 1);
    *stack = UINT64_C(7);
    encore_node_retain(stack);
    assert(encore_node_debug_ref_count(stack) == 2);
    assert(!encore_node_release(stack));
    assert(encore_node_release(stack));
    encore_node_free_released(stack);
    assert(storage.payload == UINT64_C(7));

    /*
     * Releasing a transient root into a child that is still dominated by an
     * externally rooted parent must not trial-delete the child's full graph.
     */
    void *rooted_parent = encore_node_alloc_heap(sizeof(void *));
    void *rooted_child = encore_node_alloc_heap(sizeof(void *));
    void *rooted_leaf = encore_node_alloc_heap(sizeof(uint64_t));
    connect(rooted_child, rooted_leaf);
    connect(rooted_parent, rooted_child);
    size_t scans_before_rooted_release = encore_node_debug_ern_scan_count();
    assert(!encore_node_release(rooted_leaf));
    assert(!encore_node_release(rooted_child));
    assert(encore_node_debug_ern_scan_count() == scans_before_rooted_release);
    assert(encore_node_release(rooted_parent));
    assert(encore_node_release(rooted_child));
    assert(encore_node_release(rooted_leaf));
    encore_node_free_released(rooted_leaf);
    encore_node_free_released(rooted_child);
    encore_node_free_released(rooted_parent);

    /* A root-reachable cycle survives until its final external edge drops. */
    void *cycle_a = encore_node_alloc_heap(sizeof(void *));
    void *cycle_b = encore_node_alloc_heap(sizeof(void *));
    assert(cycle_a != NULL && cycle_b != NULL);
    connect(cycle_a, cycle_b);
    connect(cycle_b, cycle_a);
    assert(encore_node_debug_edge_count(cycle_a) == 1);
    assert(encore_node_debug_edge_count(cycle_b) == 1);
    assert(!encore_node_release(cycle_b));
    assert(encore_node_release(cycle_a));
    /* Simulate the generated shallow finalizers: A -> B -> A. */
    assert(encore_node_release(cycle_b));
    assert(!encore_node_release(cycle_a));
    encore_node_free_released(cycle_b);
    encore_node_free_released(cycle_a);

    /* An edge leaving ERN is released but its externally rooted target lives. */
    void *owner_a = encore_node_alloc_heap(sizeof(void *) * 2);
    void *owner_b = encore_node_alloc_heap(sizeof(void *));
    void *shared = encore_node_alloc_heap(sizeof(uint64_t));
    assert(owner_a != NULL && owner_b != NULL && shared != NULL);
    connect(owner_a, owner_b);
    connect(owner_b, owner_a);
    connect(owner_a, shared);
    assert(!encore_node_release(owner_b));
    assert(encore_node_release(owner_a));
    assert(encore_node_release(owner_b));
    assert(!encore_node_release(owner_a));
    assert(!encore_node_release(shared));
    encore_node_free_released(owner_b);
    encore_node_free_released(owner_a);
    assert(encore_node_debug_ref_count(shared) == 1);
    assert(encore_node_release(shared));
    encore_node_free_released(shared);

    /* Replacement updates graph topology independently of edge ownership. */
    void *replace_owner = encore_node_alloc_heap(sizeof(void *));
    void *old_target = encore_node_alloc_heap(sizeof(uint64_t));
    void *new_target = encore_node_alloc_heap(sizeof(uint64_t));
    connect(replace_owner, old_target);
    encore_node_retain(new_target);
    encore_node_replace_edge(replace_owner, old_target, new_target);
    assert(encore_node_debug_edge_count(replace_owner) == 1);
    assert(!encore_node_release(old_target));
    assert(encore_node_release(old_target));
    encore_node_free_released(old_target);
    assert(encore_node_release(replace_owner));
    assert(!encore_node_release(new_target));
    encore_node_free_released(replace_owner);
    assert(encore_node_release(new_target));
    encore_node_free_released(new_target);
    assert(encore_node_debug_live_count() == 0);
    return 0;
}
