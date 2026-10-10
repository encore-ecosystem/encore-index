#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

void *__ehir_hrealloc(void *pointer, size_t bytes);
void *__ehir_hrealloc_array(void *pointer, size_t count, size_t element_size);
void __ehir_hfree(void *pointer);
void encore_heap_retain(void *pointer);

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "overflow") == 0) {
        (void)__ehir_hrealloc_array(NULL, SIZE_MAX / 2 + 1, 2);
        return 2;
    }
    if (argc == 2 && strcmp(argv[1], "header-overflow") == 0) {
        (void)__ehir_hrealloc_array(NULL, SIZE_MAX, 1);
        return 2;
    }
    assert(__ehir_hrealloc(NULL, SIZE_MAX) == NULL);
    uint64_t *first = __ehir_hrealloc_array(NULL, 2, sizeof(uint64_t));
    assert(first != NULL);
    first[0] = 13;
    first[1] = 42;
    assert(__ehir_hrealloc(first, SIZE_MAX) == NULL);
    assert(first[0] == 13 && first[1] == 42);
    encore_heap_retain(first);
    uint64_t *grown = __ehir_hrealloc_array(first, 8, sizeof(uint64_t));
    assert(grown != NULL && grown != first);
    assert(grown[0] == 13 && grown[1] == 42);
    grown[0] = 99;
    assert(first[0] == 13);
    __ehir_hfree(first);
    __ehir_hfree(grown);
    void *empty = __ehir_hrealloc_array(NULL, SIZE_MAX, 0);
    assert(empty != NULL);
    __ehir_hfree(empty);
    return 0;
}
