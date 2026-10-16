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
void *encore_node_owner_of_place(void *place);

typedef struct stack_u64_node {
    uint64_t node_prefix[8];
    uint64_t payload;
} stack_u64_node;

static void connect(void *owner, void *target) {
    encore_node_retain(target);
    encore_node_add_edge(owner, target);
}

static void check_projected_places(void) {
    unsigned char local[32] = {0};
    assert(encore_node_owner_of_place(local) == NULL);
    assert(encore_node_owner_of_place(NULL) == NULL);
    /* Multiple rotations and arbitrary deletion order must preserve every
     * address interval, including unaligned fields and both boundaries. */
    void *nodes[4096];
    for (size_t i = 0; i < 4096; ++i) {
        nodes[i] = encore_node_alloc_heap(37);
        assert(nodes[i] != NULL);
    }
    for (size_t i = 0; i < 4096; ++i) {
        unsigned char *node = nodes[i];
        assert(encore_node_owner_of_place(node) == node);
        assert(encore_node_owner_of_place(node + 1) == node);
        assert(encore_node_owner_of_place(node + 36) == node);
        assert(encore_node_owner_of_place(node - 1) == NULL);
        assert(encore_node_owner_of_place(node + 37) == NULL);
    }
    for (size_t i = 0; i < 4096; ++i) {
        size_t index = (i * 137) % 4096;
        void *node = nodes[index];
        assert(encore_node_release(node));
        assert(encore_node_owner_of_place(node) == NULL);
        encore_node_free_released(node);
    }
    /* Stack payloads and allocator address reuse must not leave stale owners. */
    stack_u64_node storage = {{0}, 0};
    for (size_t i = 0; i < 128; ++i) {
        encore_node_register_stack(&storage.payload, sizeof(storage.payload));
        assert(encore_node_owner_of_place(&storage.payload) == &storage.payload);
        assert(encore_node_release(&storage.payload));
        encore_node_free_released(&storage.payload);
        assert(encore_node_owner_of_place(&storage.payload) == NULL);
        void *heap = encore_node_alloc_heap(sizeof(uint64_t));
        assert(encore_node_owner_of_place(heap) == heap);
        assert(encore_node_release(heap));
        encore_node_free_released(heap);
    }
    assert(encore_node_debug_live_count() == 0);
}

static void check_deep_owner_chain(void) {
    void *nodes[128];
    for (size_t i = 0; i < 128; ++i) {
        nodes[i] = encore_node_alloc_heap(sizeof(void *));
        assert(nodes[i] != NULL);
        if (i > 0) connect(nodes[i - 1], nodes[i]);
    }
    size_t scans = encore_node_debug_ern_scan_count();
    for (size_t i = 1; i < 128; ++i) {
        assert(!encore_node_release(nodes[i]));
    }
    assert(encore_node_debug_ern_scan_count() == scans);
    /* Shallow finalizers recurse first, then unwind in reverse order. */
    for (size_t i = 0; i < 128; ++i) assert(encore_node_release(nodes[i]));
    for (size_t i = 128; i > 0; --i) encore_node_free_released(nodes[i - 1]);
    assert(encore_node_debug_live_count() == 0);
}

int main(void) {
    assert(encore_node_debug_live_count() == 0);
    check_projected_places();
    check_deep_owner_chain();
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
    /* Finish B's recursive finalizer before dropping A's next field. */
    encore_node_free_released(owner_b);
    assert(!encore_node_release(shared));
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
