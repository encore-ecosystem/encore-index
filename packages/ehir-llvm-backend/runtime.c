#include <stddef.h>
#if defined(__linux__)
#include <stdio.h>
#include <unistd.h>
#endif

static size_t encore_target_pointer_width = sizeof(void *) * 8u;
static size_t encore_windows_target = 0u;

void encore_llvm_set_target_pointer_width(size_t bits) {
    encore_target_pointer_width = bits == 32u ? 32u : 64u;
}

void encore_llvm_set_windows_target(size_t enabled) {
    encore_windows_target = enabled != 0u;
}

size_t encore_llvm_is_windows_target(void) {
    return encore_windows_target;
}

size_t encore_llvm_target_pointer_width(void) {
    return encore_target_pointer_width;
}

size_t encore_codegen_rss_kb(void) {
#if defined(__linux__)
    FILE *file = fopen("/proc/self/statm", "r");
    unsigned long pages = 0;
    unsigned long resident = 0;
    if (file == NULL) return 0;
    if (fscanf(file, "%lu %lu", &pages, &resident) != 2) resident = 0;
    fclose(file);
    return (size_t)(resident * (unsigned long)sysconf(_SC_PAGESIZE) / 1024ul);
#else
    return 0;
#endif
}
