#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#define SECURITY_WIN32
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#if defined(__linux__)
#include <sys/resource.h>
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#if defined(__linux__)
/*
 * Keep hosted debug programs viable when generated aggregate temporaries
 * exceed the conventional 8 MiB main-thread stack. Respect the hard limit;
 * optimized profiles normally eliminate these slots.
 */
__attribute__((constructor))
static void encore_ensure_main_stack_capacity(void) {
    const rlim_t desired = (rlim_t)64 * 1024 * 1024;
    struct rlimit limit;
    if (getrlimit(RLIMIT_STACK, &limit) != 0 || limit.rlim_cur >= desired) return;
    rlim_t next = desired;
    if (limit.rlim_max != RLIM_INFINITY && next > limit.rlim_max) {
        next = limit.rlim_max;
    }
    if (next > limit.rlim_cur) {
        limit.rlim_cur = next;
        (void)setrlimit(RLIMIT_STACK, &limit);
    }
}
#endif
#ifdef __APPLE__
#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <Security/SecureTransport.h>
#include <sys/sysctl.h>
#endif
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <direct.h>
#include <io.h>
#include <process.h>
#include <security.h>
#include <schannel.h>
#include <wincrypt.h>
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Secur32.lib")
#pragma comment(lib, "Crypt32.lib")
#pragma comment(linker, "/STACK:8388608")
#else
#include <pthread.h>
#include <spawn.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <dirent.h>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#if defined(__linux__) && !defined(__ANDROID__)
#include <openssl/err.h>
#include <openssl/ssl.h>
extern char **environ;
#elif !defined(__APPLE__)
extern char **environ;
#endif
#endif
typedef union encore_heap_block encore_heap_block;
union encore_heap_block {
    struct {
        size_t capacity;
        _Atomic size_t refs;
    } meta;
    max_align_t alignment;
};

void __ehir_hfree(void *ptr);

static void *encore_heap_alloc(size_t bytes) {
    if (bytes == 0) bytes = 1;
    encore_heap_block *block = malloc(sizeof(encore_heap_block) + bytes);
    if (block == NULL) return NULL;
    block->meta.capacity = bytes;
    atomic_init(&block->meta.refs, 1);
    return block + 1;
}

void *__ehir_hrealloc(void *ptr, size_t bytes) {
    if (ptr == NULL) return encore_heap_alloc(bytes);
    if (bytes == 0) bytes = 1;
    encore_heap_block *block = ((encore_heap_block *)ptr) - 1;
    if (bytes <= block->meta.capacity) return ptr;
    if (atomic_load_explicit(&block->meta.refs, memory_order_acquire) == 1) {
        encore_heap_block *resized = realloc(block, sizeof(encore_heap_block) + bytes);
        if (resized == NULL) return NULL;
        resized->meta.capacity = bytes;
        return resized + 1;
    }
    void *next = encore_heap_alloc(bytes);
    if (next == NULL) return NULL;
    memcpy(next, ptr, block->meta.capacity);
    atomic_fetch_sub_explicit(&block->meta.refs, 1, memory_order_acq_rel);
    return next;
}

void __ehir_hfree(void *ptr) {
    if (ptr == NULL) return;
    encore_heap_block *block = ((encore_heap_block *)ptr) - 1;
    if (atomic_fetch_sub_explicit(&block->meta.refs, 1, memory_order_acq_rel) == 1) free(block);
}

void encore_heap_retain(void *ptr) {
    if (ptr == NULL) return;
    encore_heap_block *block = ((encore_heap_block *)ptr) - 1;
    atomic_fetch_add_explicit(&block->meta.refs, 1, memory_order_relaxed);
}

bool encore_heap_release(void *ptr) {
    if (ptr == NULL) return false;
    encore_heap_block *block = ((encore_heap_block *)ptr) - 1;
    return atomic_fetch_sub_explicit(&block->meta.refs, 1, memory_order_acq_rel) == 1;
}

void encore_heap_free_released(void *ptr) {
    if (ptr == NULL) return;
    free(((encore_heap_block *)ptr) - 1);
}

#if defined(__clang__) || defined(__GNUC__)
__attribute__((visibility("hidden"), always_inline))
#endif
bool encore_heap_is_unique(void *ptr) {
    if (ptr == NULL) return true;
    encore_heap_block *block = ((encore_heap_block *)ptr) - 1;
    return atomic_load_explicit(&block->meta.refs, memory_order_acquire) == 1;
}

#include "node_runtime.c"

typedef union {
    struct {
        _Atomic size_t refs;
    } meta;
    max_align_t alignment;
} encore_box_header;

void *encore_box_alloc(size_t bytes) {
    encore_box_header *header = malloc(sizeof(encore_box_header) + bytes);
    if (header == NULL) return NULL;
    atomic_init(&header->meta.refs, 1);
    return (void *)(header + 1);
}

void encore_box_retain(void *payload) {
    if (payload == NULL) return;
    encore_box_header *header = ((encore_box_header *)payload) - 1;
    atomic_fetch_add_explicit(&header->meta.refs, 1, memory_order_relaxed);
}

void encore_box_drop(void *payload) {
    if (payload == NULL) return;
    encore_box_header *header = ((encore_box_header *)payload) - 1;
    if (atomic_fetch_sub_explicit(&header->meta.refs, 1, memory_order_acq_rel) == 1) free(header);
}

typedef struct {
    _Atomic size_t ref_count;
    size_t len;
    char data[];
} encore_str_object;

typedef struct {
    encore_str_object *object;
} encore_str;

encore_str encore_empty_str(void);
encore_str encore_from_owned_buffer(char *buffer, size_t len);

static struct {
    _Atomic size_t ref_count;
    size_t len;
    char data[1];
} g_empty_str_object = {.ref_count = 0, .len = 0, .data = {0}};

char *encore_str_data(encore_str value) {
    if (value.object == NULL) {
        return g_empty_str_object.data;
    }
    return value.object->data;
}

size_t encore_str_size(encore_str value) {
    if (value.object == NULL) {
        return 0;
    }
    return value.object->len;
}

typedef struct {
    uint64_t hash;
    size_t len;
    char *data;
} encore_strset_entry;

typedef struct {
    size_t len;
    size_t cap;
    encore_strset_entry *entries;
} encore_strset;

static uint64_t encore_strset_hash(const char *data, size_t len) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t index = 0; index < len; ++index) {
        hash ^= (unsigned char)data[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash == 0 ? 1 : hash;
}

static bool encore_strset_rehash(encore_strset *set, size_t next_cap) {
    encore_strset_entry *next = calloc(next_cap, sizeof(encore_strset_entry));
    if (next == NULL) return false;
    for (size_t index = 0; index < set->cap; ++index) {
        encore_strset_entry entry = set->entries[index];
        if (entry.data == NULL) continue;
        size_t slot = (size_t)(entry.hash % next_cap);
        while (next[slot].data != NULL) slot = (slot + 1) % next_cap;
        next[slot] = entry;
    }
    free(set->entries);
    set->entries = next;
    set->cap = next_cap;
    return true;
}

void *encore_strset_new(void) {
    encore_strset *set = calloc(1, sizeof(encore_strset));
    if (set == NULL) return NULL;
    if (!encore_strset_rehash(set, 64)) {
        free(set);
        return NULL;
    }
    return set;
}

bool encore_strset_insert(void *raw_set, encore_str value) {
    encore_strset *set = raw_set;
    if (set == NULL) return true;
    if ((set->len + 1) * 10 >= set->cap * 7) {
        if (!encore_strset_rehash(set, set->cap * 2)) return true;
    }
    const char *data = encore_str_data(value);
    size_t len = encore_str_size(value);
    uint64_t hash = encore_strset_hash(data, len);
    size_t slot = (size_t)(hash % set->cap);
    while (set->entries[slot].data != NULL) {
        encore_strset_entry *entry = &set->entries[slot];
        if (entry->hash == hash && entry->len == len && memcmp(entry->data, data, len) == 0) {
            return false;
        }
        slot = (slot + 1) % set->cap;
    }
    char *copy = malloc(len + 1);
    if (copy == NULL) return true;
    if (len > 0) memcpy(copy, data, len);
    copy[len] = '\0';
    set->entries[slot] = (encore_strset_entry){.hash = hash, .len = len, .data = copy};
    set->len += 1;
    return true;
}

bool encore_strset_contains(void *raw_set, encore_str value) {
    encore_strset *set = raw_set;
    if (set == NULL || set->cap == 0) return false;
    const char *data = encore_str_data(value);
    size_t len = encore_str_size(value);
    uint64_t hash = encore_strset_hash(data, len);
    size_t slot = (size_t)(hash % set->cap);
    while (set->entries[slot].data != NULL) {
        encore_strset_entry *entry = &set->entries[slot];
        if (entry->hash == hash && entry->len == len && memcmp(entry->data, data, len) == 0) {
            return true;
        }
        slot = (slot + 1) % set->cap;
    }
    return false;
}

void encore_strset_free(void *raw_set) {
    encore_strset *set = raw_set;
    if (set == NULL) return;
    for (size_t index = 0; index < set->cap; ++index) free(set->entries[index].data);
    free(set->entries);
    free(set);
}

typedef struct {
    uint64_t hash;
    size_t len;
    char *data;
    size_t value;
} encore_strmap_entry;

typedef struct {
    size_t len;
    size_t cap;
    encore_strmap_entry *entries;
} encore_strmap;

static bool encore_strmap_rehash(encore_strmap *map, size_t next_cap) {
    encore_strmap_entry *next = calloc(next_cap, sizeof(encore_strmap_entry));
    if (next == NULL) return false;
    for (size_t index = 0; index < map->cap; ++index) {
        encore_strmap_entry entry = map->entries[index];
        if (entry.data == NULL) continue;
        size_t slot = (size_t)(entry.hash % next_cap);
        while (next[slot].data != NULL) slot = (slot + 1) % next_cap;
        next[slot] = entry;
    }
    free(map->entries);
    map->entries = next;
    map->cap = next_cap;
    return true;
}

void *encore_strmap_new(void) {
    encore_strmap *map = calloc(1, sizeof(encore_strmap));
    if (map == NULL) return NULL;
    if (!encore_strmap_rehash(map, 64)) {
        free(map);
        return NULL;
    }
    return map;
}

void encore_strmap_put(void *raw_map, encore_str key, size_t value) {
    encore_strmap *map = raw_map;
    if (map == NULL) return;
    if ((map->len + 1) * 10 >= map->cap * 7 &&
        !encore_strmap_rehash(map, map->cap * 2)) return;
    const char *data = encore_str_data(key);
    size_t len = encore_str_size(key);
    uint64_t hash = encore_strset_hash(data, len);
    size_t slot = (size_t)(hash % map->cap);
    while (map->entries[slot].data != NULL) {
        encore_strmap_entry *entry = &map->entries[slot];
        if (entry->hash == hash && entry->len == len && memcmp(entry->data, data, len) == 0) {
            entry->value = value;
            return;
        }
        slot = (slot + 1) % map->cap;
    }
    char *copy = malloc(len + 1);
    if (copy == NULL) return;
    if (len > 0) memcpy(copy, data, len);
    copy[len] = '\0';
    map->entries[slot] = (encore_strmap_entry){hash, len, copy, value};
    map->len += 1;
}

size_t encore_strmap_get(void *raw_map, encore_str key) {
    encore_strmap *map = raw_map;
    if (map == NULL || map->cap == 0) return 0;
    const char *data = encore_str_data(key);
    size_t len = encore_str_size(key);
    uint64_t hash = encore_strset_hash(data, len);
    size_t slot = (size_t)(hash % map->cap);
    while (map->entries[slot].data != NULL) {
        encore_strmap_entry *entry = &map->entries[slot];
        if (entry->hash == hash && entry->len == len && memcmp(entry->data, data, len) == 0) {
            return entry->value;
        }
        slot = (slot + 1) % map->cap;
    }
    return 0;
}

void encore_strmap_free(void *raw_map) {
    encore_strmap *map = raw_map;
    if (map == NULL) return;
    for (size_t index = 0; index < map->cap; ++index) free(map->entries[index].data);
    free(map->entries);
    free(map);
}

typedef struct {
    size_t len;
    size_t cap;
    encore_str_object *object;
    bool failed;
} encore_text_builder;

static _Thread_local bool encore_text_builder_suppressed = false;
static _Thread_local bool encore_translation_diagnostic_json = false;

void encore_text_builder_set_suppressed(bool suppressed) {
    encore_text_builder_suppressed = suppressed;
}

void encore_translation_diagnostic_set_json(bool enabled) {
    encore_translation_diagnostic_json = enabled;
}

bool encore_translation_diagnostic_is_json(void) {
    return encore_translation_diagnostic_json;
}

static bool encore_text_builder_reserve(encore_text_builder *builder, size_t additional) {
    if (builder == NULL || additional > SIZE_MAX - builder->len) return false;
    size_t required = builder->len + additional;
    if (required <= builder->cap) return true;
    size_t next_cap = builder->cap == 0 ? 256 : builder->cap;
    while (next_cap < required) {
        if (next_cap > SIZE_MAX / 2) {
            next_cap = required;
            break;
        }
        next_cap *= 2;
    }
    if (next_cap > SIZE_MAX - sizeof(encore_str_object) - 1) {
        builder->failed = true;
        return false;
    }
    encore_str_object *next = realloc(builder->object,
        sizeof(encore_str_object) + next_cap + 1);
    if (next == NULL) return false;
    if (builder->object == NULL) atomic_init(&next->ref_count, 1);
    builder->object = next;
    builder->cap = next_cap;
    return true;
}

void *encore_text_builder_new(void) {
    return calloc(1, sizeof(encore_text_builder));
}

void encore_text_builder_append(void *raw_builder, encore_str value) {
    encore_text_builder *builder = raw_builder;
    if (builder == NULL || builder->failed) return;
    if (encore_text_builder_suppressed) return;
    size_t len = encore_str_size(value);
    if (!encore_text_builder_reserve(builder, len)) {
        builder->failed = true;
        return;
    }
    if (len > 0) memcpy(builder->object->data + builder->len, encore_str_data(value), len);
    builder->len += len;
}

void encore_text_builder_append_builder(void *raw_builder, void *raw_other) {
    encore_text_builder *builder = raw_builder;
    encore_text_builder *other = raw_other;
    if (builder == NULL || other == NULL || builder == other) return;
    if (encore_text_builder_suppressed) {
        free(other->object);
        free(other);
        return;
    }
    if (other->failed) {
        builder->failed = true;
    } else if (encore_text_builder_reserve(builder, other->len)) {
        if (other->len > 0) memcpy(builder->object->data + builder->len,
            other->object->data, other->len);
        builder->len += other->len;
    } else {
        builder->failed = true;
    }
    free(other->object);
    free(other);
}

encore_str encore_text_builder_finish(void *raw_builder) {
    encore_text_builder *builder = raw_builder;
    if (builder == NULL) return encore_empty_str();
    encore_str_object *object = builder->object;
    size_t len = builder->len;
    bool failed = builder->failed;
    free(builder);
    if (failed || object == NULL) {
        free(object);
        return encore_empty_str();
    }
    object->len = len;
    object->data[len] = '\0';
    return (encore_str){.object = object};
}

void encore_text_builder_discard(void *raw_builder) {
    encore_text_builder *builder = raw_builder;
    if (builder == NULL) return;
    free(builder->object);
    free(builder);
}

encore_str encore_empty_str(void) {
    return (encore_str){.object = (encore_str_object *)&g_empty_str_object};
}

char *encore_to_cstr(encore_str value) {
    size_t len = encore_str_size(value);
    char *buffer = malloc(len + 1);
    if (buffer == NULL) {
        return NULL;
    }

    char *data = encore_str_data(value);
    if (len > 0 && data != NULL) {
        memcpy(buffer, data, len);
    }
    buffer[len] = '\0';
    return buffer;
}

encore_str encore_from_owned_buffer(char *buffer, size_t len) {
    if (buffer == NULL) {
        return encore_empty_str();
    }
    /* Strings have their own precise refcount, so they do not need the
     * alias-tolerant aggregate arena. Releasing them eagerly is essential for
     * compiler workloads that create millions of temporary IR fragments. */
    encore_str_object *object = malloc(sizeof(encore_str_object) + len + 1);
    if (object == NULL) {
        free(buffer);
        return encore_empty_str();
    }
    atomic_init(&object->ref_count, 1);
    object->len = len;
    if (len > 0) {
        memcpy(object->data, buffer, len);
    }
    object->data[len] = '\0';
    free(buffer);
    return (encore_str){.object = object};
}

static int encore_hex_digit(unsigned char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static size_t encore_utf8_encode(uint32_t value, char *out) {
    if (value <= 0x7f) { out[0] = (char)value; return 1; }
    if (value <= 0x7ff) {
        out[0] = (char)(0xc0 | (value >> 6));
        out[1] = (char)(0x80 | (value & 0x3f));
        return 2;
    }
    if (value >= 0xd800 && value <= 0xdfff) return 0;
    if (value <= 0xffff) {
        out[0] = (char)(0xe0 | (value >> 12));
        out[1] = (char)(0x80 | ((value >> 6) & 0x3f));
        out[2] = (char)(0x80 | (value & 0x3f));
        return 3;
    }
    if (value <= 0x10ffff) {
        out[0] = (char)(0xf0 | (value >> 18));
        out[1] = (char)(0x80 | ((value >> 12) & 0x3f));
        out[2] = (char)(0x80 | ((value >> 6) & 0x3f));
        out[3] = (char)(0x80 | (value & 0x3f));
        return 4;
    }
    return 0;
}

encore_str encore_str_from_codepoint(size_t value) {
    char *buffer = malloc(5);
    if (buffer == NULL) return encore_empty_str();
    size_t encoded = encore_utf8_encode((uint32_t)value, buffer);
    if (encoded == 0) {
        free(buffer);
        return encore_empty_str();
    }
    buffer[encoded] = '\0';
    return encore_from_owned_buffer(buffer, encoded);
}

encore_str encore_llvm_float_literal(encore_str value, size_t is_f32) {
    size_t len = encore_str_size(value);
    char *input = malloc(len + 1);
    if (input == NULL) return encore_empty_str();
    memcpy(input, encore_str_data(value), len);
    input[len] = '\0';
    double parsed = strtod(input, NULL);
    free(input);
    char *buffer = malloc(32);
    if (buffer == NULL) return encore_empty_str();
    union { double number; uint64_t bits; } encoded;
    encoded.number = is_f32 ? (double)(float)parsed : parsed;
    int written = snprintf(buffer, 32, "0x%016" PRIX64, encoded.bits);
    if (written <= 0) {
        free(buffer);
        return encore_empty_str();
    }
    return encore_from_owned_buffer(buffer, (size_t)written);
}

encore_str encore_unescape_string_literal(encore_str value) {
    const unsigned char *input = (const unsigned char *)encore_str_data(value);
    size_t len = encore_str_size(value);
    char *output = malloc(len + 1);
    if (output == NULL) return encore_empty_str();
    size_t read = 0, written = 0;
    while (read < len) {
        if (input[read] != '\\' || read + 1 >= len) {
            output[written++] = (char)input[read++];
            continue;
        }
        unsigned char escape = input[read + 1];
        if (escape == 'n' || escape == 't' || escape == 'r' || escape == '\\' || escape == '"') {
            output[written++] = escape == 'n' ? '\n' : escape == 't' ? '\t' : escape == 'r' ? '\r' : (char)escape;
            read += 2;
            continue;
        }
        if (escape == 'x' && read + 3 < len) {
            int high = encore_hex_digit(input[read + 2]);
            int low = encore_hex_digit(input[read + 3]);
            if (high >= 0 && low >= 0) {
                output[written++] = (char)((high << 4) | low);
                read += 4;
                continue;
            }
        }
        if (escape == 'u' && read + 3 < len && input[read + 2] == '{') {
            size_t end = read + 3;
            uint32_t codepoint = 0;
            size_t digits = 0;
            while (end < len && input[end] != '}') {
                int digit = encore_hex_digit(input[end]);
                if (digit < 0 || codepoint > 0x10ffffu / 16u) break;
                codepoint = codepoint * 16u + (uint32_t)digit;
                digits += 1;
                end += 1;
            }
            if (digits > 0 && end < len && input[end] == '}') {
                size_t encoded = encore_utf8_encode(codepoint, output + written);
                if (encoded > 0) {
                    written += encoded;
                    read = end + 1;
                    continue;
                }
            }
        }
        output[written++] = '\\';
        output[written++] = (char)escape;
        read += 2;
    }
    output[written] = '\0';
    return encore_from_owned_buffer(output, written);
}

encore_str encore_from_cstr_copy(const char *value) {
    if (value == NULL) {
        return encore_empty_str();
    }

    size_t len = strlen(value);
    char *buffer = malloc(len + 1);
    if (buffer == NULL) {
        return encore_empty_str();
    }
    memcpy(buffer, value, len + 1);
    return encore_from_owned_buffer(buffer, len);
}

void *encore_str_from_cstr(const char *value) {
    encore_str result = encore_from_cstr_copy(value);
    return result.object;
}

typedef struct {
    encore_str *ptr;
    size_t len;
    size_t cap;
} encore_str_vec;

encore_str encore_str_join_lines(encore_str_vec lines) {
    size_t total = lines.len > 0 ? lines.len - 1 : 0;
    for (size_t index = 0; index < lines.len; index += 1) {
        total += encore_str_size(lines.ptr[index]);
    }
    char *buffer = malloc(total + 1);
    if (buffer == NULL) {
        return encore_empty_str();
    }
    size_t offset = 0;
    for (size_t index = 0; index < lines.len; index += 1) {
        if (index > 0) {
            buffer[offset] = '\n';
            offset += 1;
        }
        size_t len = encore_str_size(lines.ptr[index]);
        if (len > 0) {
            memcpy(buffer + offset, encore_str_data(lines.ptr[index]), len);
            offset += len;
        }
    }
    buffer[offset] = '\0';
    return encore_from_owned_buffer(buffer, offset);
}

encore_str encore_str_join_lines_parts(size_t raw_ptr, size_t len) {
    encore_str_vec lines = {
        .ptr = (encore_str *)(uintptr_t)raw_ptr,
        .len = len,
        .cap = len,
    };
    return encore_str_join_lines(lines);
}

encore_str encore_str_join_parts(size_t raw_ptr, size_t len) {
    encore_str *parts = (encore_str *)(uintptr_t)raw_ptr;
    size_t total = 0;
    for (size_t index = 0; index < len; index += 1) {
        size_t part_len = encore_str_size(parts[index]);
        if (part_len > SIZE_MAX - total) return encore_empty_str();
        total += part_len;
    }
    char *buffer = malloc(total + 1);
    if (buffer == NULL) return encore_empty_str();
    size_t offset = 0;
    for (size_t index = 0; index < len; index += 1) {
        size_t part_len = encore_str_size(parts[index]);
        if (part_len > 0) {
            memcpy(buffer + offset, encore_str_data(parts[index]), part_len);
            offset += part_len;
        }
    }
    buffer[offset] = '\0';
    return encore_from_owned_buffer(buffer, offset);
}

void encore_str_retain(encore_str value) {
    if (value.object == NULL || atomic_load_explicit(&value.object->ref_count, memory_order_relaxed) == 0) {
        return;
    }
    atomic_fetch_add_explicit(&value.object->ref_count, 1, memory_order_relaxed);
}

void encore_str_drop(encore_str value) {
    if (value.object == NULL || atomic_load_explicit(&value.object->ref_count, memory_order_relaxed) == 0) {
        return;
    }
    if (atomic_fetch_sub_explicit(&value.object->ref_count, 1, memory_order_acq_rel) == 1) free(value.object);
}

static encore_str encore_format(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (needed < 0) {
        return encore_empty_str();
    }

    char *buffer = malloc((size_t)needed + 1);
    if (buffer == NULL) {
        return encore_empty_str();
    }

    va_start(args, fmt);
    int written = vsnprintf(buffer, (size_t)needed + 1, fmt, args);
    va_end(args);
    if (written < 0) {
        free(buffer);
        return encore_empty_str();
    }

    return encore_from_owned_buffer(buffer, (size_t)written);
}

bool encore_str_eq(encore_str lhs, encore_str rhs) {
    size_t lhs_len = encore_str_size(lhs);
    size_t rhs_len = encore_str_size(rhs);
    if (lhs_len != rhs_len) {
        return false;
    }
    if (lhs_len == 0) {
        return true;
    }
    char *lhs_data = encore_str_data(lhs);
    char *rhs_data = encore_str_data(rhs);
    if (lhs_data == NULL || rhs_data == NULL) {
        return false;
    }
    return memcmp(lhs_data, rhs_data, lhs_len) == 0;
}

size_t encore_str_len(encore_str value) {
    return encore_str_size(value);
}

uint8_t encore_str_byte_at(encore_str value, size_t index) {
    size_t len = encore_str_size(value);
    char *data = encore_str_data(value);
    if (data == NULL || index >= len) {
        return 0;
    }
    return (uint8_t)data[index];
}

size_t encore_str_find_byte_from(encore_str value, uint8_t needle, size_t start) {
    size_t len = encore_str_size(value);
    const unsigned char *data = (const unsigned char *)encore_str_data(value);
    if (data == NULL || start >= len) return len;
    const unsigned char *found = memchr(data + start, needle, len - start);
    return found == NULL ? len : (size_t)(found - data);
}

size_t encore_str_find_control_from(encore_str value, size_t start) {
    size_t len = encore_str_size(value);
    const unsigned char *data = (const unsigned char *)encore_str_data(value);
    if (data == NULL || start >= len) return len;
    for (size_t index = start; index < len; ++index) {
        if (data[index] < 0x20) return index;
    }
    return len;
}

size_t encore_str_line_start_byte(encore_str value, size_t target_line) {
    size_t len = encore_str_size(value);
    const unsigned char *data = (const unsigned char *)encore_str_data(value);
    if (target_line == 0) return 0;
    if (data == NULL) return len + 1;
    size_t line = 0;
    for (size_t index = 0; index < len; ++index) {
        if (data[index] == '\n') {
            line += 1;
            if (line == target_line) return index + 1;
        }
    }
    return len + 1;
}

static size_t encore_utf8_char_width(uint8_t lead) {
    if ((lead & 0x80u) == 0u) {
        return 1;
    }
    if ((lead & 0xE0u) == 0xC0u) {
        return 2;
    }
    if ((lead & 0xF0u) == 0xE0u) {
        return 3;
    }
    if ((lead & 0xF8u) == 0xF0u) {
        return 4;
    }
    return 1;
}

static encore_str encore_str_copy_range(encore_str value, size_t start, size_t slice_len) {
    size_t len = encore_str_size(value);
    char *data = encore_str_data(value);
    if (data == NULL || start >= len) {
        return encore_empty_str();
    }

    size_t remaining = len - start;
    size_t actual_len = slice_len < remaining ? slice_len : remaining;
    char *buffer = malloc(actual_len + 1);
    if (buffer == NULL) {
        return encore_empty_str();
    }

    memcpy(buffer, data + start, actual_len);
    return encore_from_owned_buffer(buffer, actual_len);
}

size_t encore_str_char_width_at_byte(encore_str value, size_t byte_index) {
    size_t len = encore_str_size(value);
    char *data = encore_str_data(value);
    if (data == NULL || byte_index >= len) return 0;
    size_t width = encore_utf8_char_width((uint8_t)data[byte_index]);
    return byte_index + width <= len ? width : 1;
}

encore_str encore_str_char_at_byte(encore_str value, size_t byte_index) {
    size_t width = encore_str_char_width_at_byte(value, byte_index);
    if (width == 0) return encore_empty_str();
    return encore_str_copy_range(value, byte_index, width);
}

encore_str encore_str_slice_bytes(encore_str value, size_t start, size_t byte_len) {
    return encore_str_copy_range(value, start, byte_len);
}

size_t encore_str_char_len(encore_str value) {
    size_t len = encore_str_size(value);
    char *data = encore_str_data(value);
    if (data == NULL || len == 0) {
        return 0;
    }

    size_t chars = 0;
    size_t i = 0;
    while (i < len) {
        size_t width = encore_utf8_char_width((uint8_t)data[i]);
        if (i + width > len) {
            width = 1;
        }
        i += width;
        chars += 1;
    }
    return chars;
}

encore_str encore_str_char_at(encore_str value, size_t index) {
    size_t len = encore_str_size(value);
    char *data = encore_str_data(value);
    if (data == NULL || len == 0) {
        return encore_empty_str();
    }

    size_t i = 0;
    size_t char_index = 0;
    while (i < len) {
        size_t width = encore_utf8_char_width((uint8_t)data[i]);
        if (i + width > len) {
            width = 1;
        }
        if (char_index == index) {
            return encore_str_copy_range(value, i, width);
        }
        i += width;
        char_index += 1;
    }
    return encore_empty_str();
}

encore_str encore_str_slice_chars(encore_str value, size_t start, size_t char_len) {
    size_t len = encore_str_size(value);
    char *data = encore_str_data(value);
    if (data == NULL || len == 0 || char_len == 0) {
        return encore_empty_str();
    }

    size_t i = 0;
    size_t char_index = 0;
    size_t start_byte = len;
    size_t end_byte = len;

    while (i < len) {
        if (char_index == start) {
            start_byte = i;
            break;
        }
        size_t width = encore_utf8_char_width((uint8_t)data[i]);
        if (i + width > len) {
            width = 1;
        }
        i += width;
        char_index += 1;
    }

    if (start_byte == len) {
        return encore_empty_str();
    }

    i = start_byte;
    size_t taken = 0;
    while (i < len && taken < char_len) {
        size_t width = encore_utf8_char_width((uint8_t)data[i]);
        if (i + width > len) {
            width = 1;
        }
        i += width;
        taken += 1;
    }
    end_byte = i;
    return encore_str_copy_range(value, start_byte, end_byte - start_byte);
}

encore_str encore_str_slice(encore_str value, size_t start, size_t slice_len) {
    return encore_str_copy_range(value, start, slice_len);
}

encore_str encore_str_concat(encore_str lhs, encore_str rhs) {
    size_t lhs_len = encore_str_size(lhs);
    size_t rhs_len = encore_str_size(rhs);
    char *lhs_data = encore_str_data(lhs);
    char *rhs_data = encore_str_data(rhs);
    size_t total_len = lhs_len + rhs_len;
    char *buffer = malloc(total_len + 1);
    if (buffer == NULL) {
        return encore_empty_str();
    }

    if (lhs_data != NULL && lhs_len > 0) {
        memcpy(buffer, lhs_data, lhs_len);
    }
    if (rhs_data != NULL && rhs_len > 0) {
        memcpy(buffer + lhs_len, rhs_data, rhs_len);
    }

    return encore_from_owned_buffer(buffer, total_len);
}

encore_str encore_symbol_sanitize(encore_str value) {
    size_t len = encore_str_size(value);
    if (len == 0) return encore_from_cstr_copy("_");
    const unsigned char *data = (const unsigned char *)encore_str_data(value);
    char *buffer = malloc(len + 1);
    if (buffer == NULL) return encore_empty_str();
    for (size_t index = 0; index < len; ++index) {
        unsigned char ch = data[index];
        bool valid = ch == '_' || (ch >= '0' && ch <= '9') ||
                     (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
        buffer[index] = valid ? (char)ch : '_';
    }
    buffer[len] = '\0';
    return encore_from_owned_buffer(buffer, len);
}

encore_str encore_fmt_u64(uint64_t value) {
    return encore_format("%" PRIu64, value);
}

encore_str encore_fmt_i64(int64_t value) {
    return encore_format("%" PRId64, value);
}

encore_str encore_fmt_f64(double value) {
    return encore_format("%.17g", value);
}
