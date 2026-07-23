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
#ifndef PATH_MAX
#define PATH_MAX 4096
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

typedef struct {
#ifdef _WIN32
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE condition;
#else
    pthread_mutex_t lock;
    pthread_cond_t condition;
#endif
    bool signaled;
} encore_async_event;

typedef void (*encore_thread_entry)(void *);
typedef void (*encore_thread_cleanup)(void *);

bool encore_terminal_is_tty(int32_t fd) {
#ifdef _WIN32
    return fd >= 0 && _isatty(fd) != 0;
#else
    return fd >= 0 && isatty(fd) != 0;
#endif
}

typedef struct {
#ifdef _WIN32
    HANDLE thread;
#else
    pthread_t thread;
#endif
    encore_thread_entry entry;
    encore_thread_cleanup cleanup;
    void *payload;
    bool started;
    bool joined;
    atomic_size_t references;
#ifdef _WIN32
    CRITICAL_SECTION join_lock;
#else
    pthread_mutex_t join_lock;
#endif
} encore_thread_task;

#ifdef _WIN32
static DWORD WINAPI encore_thread_main(LPVOID raw) {
#else
static void *encore_thread_main(void *raw) {
#endif
    encore_thread_task *task = (encore_thread_task *)raw;
    task->entry(task->payload);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

size_t encore_thread_spawn(encore_thread_entry entry, void *payload, encore_thread_cleanup cleanup) {
    if (entry == NULL) return 0;
    encore_thread_task *task = malloc(sizeof(*task));
    if (task == NULL) return 0;
    task->entry = entry;
    task->cleanup = cleanup;
    task->payload = payload;
    task->started = false;
    task->joined = false;
    atomic_init(&task->references, 1);
#ifdef _WIN32
    InitializeCriticalSection(&task->join_lock);
#else
    pthread_mutex_init(&task->join_lock, NULL);
#endif
#ifdef _WIN32
    task->thread = CreateThread(NULL, 0, encore_thread_main, task, 0, NULL);
    if (task->thread != NULL) task->started = true;
#else
    if (pthread_create(&task->thread, NULL, encore_thread_main, task) == 0) task->started = true;
#endif
    if (!task->started) { task->entry(task->payload); task->joined = true; }
    return (size_t)(uintptr_t)task;
}

void *encore_thread_join(size_t token) {
    encore_thread_task *task = (encore_thread_task *)(uintptr_t)token;
    if (task == NULL) return NULL;
#ifdef _WIN32
    EnterCriticalSection(&task->join_lock);
    if (!task->joined && task->started) {
        WaitForSingleObject(task->thread, INFINITE); CloseHandle(task->thread); task->joined = true;
    }
    LeaveCriticalSection(&task->join_lock);
#else
    pthread_mutex_lock(&task->join_lock);
    if (!task->joined && task->started) { pthread_join(task->thread, NULL); task->joined = true; }
    pthread_mutex_unlock(&task->join_lock);
#endif
    return task->payload;
}

void encore_thread_retain(size_t token) {
    encore_thread_task *task = (encore_thread_task *)(uintptr_t)token;
    if (task != NULL) atomic_fetch_add_explicit(&task->references, 1, memory_order_relaxed);
}

void encore_thread_release(size_t token) {
    encore_thread_task *task = (encore_thread_task *)(uintptr_t)token;
    if (task == NULL || atomic_fetch_sub_explicit(&task->references, 1, memory_order_acq_rel) != 1) return;
    encore_thread_join(token);
    if (task->cleanup != NULL) task->cleanup(task->payload);
#ifdef _WIN32
    DeleteCriticalSection(&task->join_lock);
#else
    pthread_mutex_destroy(&task->join_lock);
#endif
    free(task);
}

size_t encore_thread_available_parallelism(void) {
#ifdef _WIN32
    DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return count == 0 ? 1 : (size_t)count;
#elif defined(__APPLE__)
    int count = 0;
    size_t size = sizeof(count);
    if (sysctlbyname("hw.logicalcpu", &count, &size, NULL, 0) != 0 || count < 1) return 1;
    return (size_t)count;
#else
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count < 1 ? 1 : (size_t)count;
#endif
}

size_t encore_async_waker_new(void) {
    encore_async_event *event = calloc(1, sizeof(*event));
    if (event == NULL) return 0;
#ifdef _WIN32
    InitializeCriticalSection(&event->lock);
    InitializeConditionVariable(&event->condition);
#else
    if (pthread_mutex_init(&event->lock, NULL) != 0) { free(event); return 0; }
    if (pthread_cond_init(&event->condition, NULL) != 0) {
        pthread_mutex_destroy(&event->lock);
        free(event);
        return 0;
    }
#endif
    return (size_t)(uintptr_t)event;
}

void encore_async_waker_wake(size_t token) {
    encore_async_event *event = (encore_async_event *)(uintptr_t)token;
    if (event == NULL) return;
#ifdef _WIN32
    EnterCriticalSection(&event->lock);
    event->signaled = true;
    WakeAllConditionVariable(&event->condition);
    LeaveCriticalSection(&event->lock);
#else
    pthread_mutex_lock(&event->lock);
    event->signaled = true;
    pthread_cond_broadcast(&event->condition);
    pthread_mutex_unlock(&event->lock);
#endif
}

void encore_async_waker_wait(size_t token) {
    encore_async_event *event = (encore_async_event *)(uintptr_t)token;
    if (event == NULL) return;
#ifdef _WIN32
    EnterCriticalSection(&event->lock);
    while (!event->signaled) SleepConditionVariableCS(&event->condition, &event->lock, INFINITE);
    event->signaled = false;
    LeaveCriticalSection(&event->lock);
#else
    pthread_mutex_lock(&event->lock);
    while (!event->signaled) pthread_cond_wait(&event->condition, &event->lock);
    event->signaled = false;
    pthread_mutex_unlock(&event->lock);
#endif
}

void encore_async_waker_drop(size_t token) {
    encore_async_event *event = (encore_async_event *)(uintptr_t)token;
    if (event == NULL) return;
#ifdef _WIN32
    DeleteCriticalSection(&event->lock);
#else
    pthread_cond_destroy(&event->condition);
    pthread_mutex_destroy(&event->lock);
#endif
    free(event);
}

void *__ehir_pcast(size_t value) {
    return (void *)(uintptr_t)value;
}

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

static encore_str encore_empty_str(void);
static encore_str encore_from_owned_buffer(char *buffer, size_t len);

static struct {
    _Atomic size_t ref_count;
    size_t len;
    char data[1];
} g_empty_str_object = {.ref_count = 0, .len = 0, .data = {0}};

static char *encore_str_data(encore_str value) {
    if (value.object == NULL) {
        return g_empty_str_object.data;
    }
    return value.object->data;
}

static size_t encore_str_size(encore_str value) {
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

static encore_str encore_empty_str(void) {
    return (encore_str){.object = (encore_str_object *)&g_empty_str_object};
}

static char *encore_to_cstr(encore_str value) {
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

static encore_str encore_from_owned_buffer(char *buffer, size_t len) {
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

static encore_str encore_from_cstr_copy(const char *value) {
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

encore_str encore_os_core_dir(void) {
    const char *configured = getenv("ENCORE_CORE_DIR");
    if (configured != NULL && configured[0] != '\0') {
        return encore_from_cstr_copy(configured);
    }

    const char *home = getenv("ENCORE_HOME");
    if (home != NULL && home[0] != '\0') {
        size_t len = strlen(home);
        const char *suffix = "/lib/encore/index/core";
        char *path = malloc(len + strlen(suffix) + 1);
        if (path != NULL) {
            memcpy(path, home, len);
            memcpy(path + len, suffix, strlen(suffix) + 1);
            return encore_from_owned_buffer(path, len + strlen(suffix));
        }
    }

    char executable[PATH_MAX];
    size_t executable_len = 0;
#ifdef _WIN32
    DWORD written = GetModuleFileNameA(NULL, executable, (DWORD)sizeof(executable));
    if (written > 0 && written < sizeof(executable)) executable_len = (size_t)written;
#elif defined(__APPLE__)
    uint32_t capacity = (uint32_t)sizeof(executable);
    if (_NSGetExecutablePath(executable, &capacity) == 0) executable_len = strlen(executable);
#else
    ssize_t written = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (written > 0) { executable[written] = '\0'; executable_len = (size_t)written; }
#endif
    if (executable_len > 0) {
        char *separator = strrchr(executable, '/');
#ifdef _WIN32
        char *backslash = strrchr(executable, '\\');
        if (backslash != NULL && (separator == NULL || backslash > separator)) separator = backslash;
#endif
        if (separator != NULL) {
            size_t bin_dir_len = (size_t)(separator - executable);
            const char *suffix = "/../lib/encore/index/core";
            char *candidate = malloc(bin_dir_len + strlen(suffix) + 1);
            if (candidate != NULL) {
                memcpy(candidate, executable, bin_dir_len);
                memcpy(candidate + bin_dir_len, suffix, strlen(suffix) + 1);
                char manifest[PATH_MAX];
                int length = snprintf(manifest, sizeof(manifest), "%s/encore.toml", candidate);
                struct stat info;
                if (length > 0 && (size_t)length < sizeof(manifest) && stat(manifest, &info) == 0) {
                    return encore_from_owned_buffer(candidate, bin_dir_len + strlen(suffix));
                }
                free(candidate);
            }
        }
    }

    const char *source = __FILE__;
    const char *separator = strrchr(source, '/');
#ifdef _WIN32
    const char *backslash = strrchr(source, '\\');
    if (backslash != NULL && (separator == NULL || backslash > separator)) separator = backslash;
#endif
    if (separator == NULL) return encore_from_cstr_copy(".");
    size_t len = (size_t)(separator - source);
    char *buffer = malloc(len + 1);
    if (buffer == NULL) return encore_empty_str();
    memcpy(buffer, source, len);
    buffer[len] = '\0';
    return encore_from_owned_buffer(buffer, len);
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

uint64_t encore_clock_ms(uint8_t kind) {
#ifdef _WIN32
    if (kind == 0) {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER value;
        value.LowPart = ft.dwLowDateTime;
        value.HighPart = ft.dwHighDateTime;
        return (uint64_t)(value.QuadPart / 10000ULL);
    }

    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    if (QueryPerformanceFrequency(&frequency) && QueryPerformanceCounter(&counter) && frequency.QuadPart > 0) {
        return (uint64_t)((counter.QuadPart * 1000ULL) / frequency.QuadPart);
    }
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clockid_t clock_kind = kind == 0 ? CLOCK_REALTIME : CLOCK_MONOTONIC;
    if (clock_gettime(clock_kind, &ts) == 0) {
        return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
    }

    struct timeval tv;
    if (gettimeofday(&tv, NULL) == 0) {
        return ((uint64_t)tv.tv_sec * 1000ULL) + ((uint64_t)tv.tv_usec / 1000ULL);
    }
    return 0ULL;
#endif
}

bool encore_sleep_ms(uint64_t ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
    return true;
#else
    struct timespec req;
    req.tv_sec = (time_t)(ms / 1000ULL);
    req.tv_nsec = (long)((ms % 1000ULL) * 1000000ULL);

    while (nanosleep(&req, &req) != 0) {
        if (errno != EINTR) {
            return false;
        }
    }
    return true;
#endif
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

encore_str encore_io_read(int32_t fd, size_t max_bytes) {
    if (fd < 0 || max_bytes == 0 || max_bytes > SIZE_MAX - 1) {
        return encore_empty_str();
    }

    char *buffer = malloc(max_bytes + 1);
    if (buffer == NULL) {
        return encore_empty_str();
    }

#ifdef _WIN32
    size_t request = max_bytes > UINT_MAX ? UINT_MAX : max_bytes;
    int bytes_read = _read(fd, buffer, (unsigned int)request);
    if (bytes_read <= 0) {
        free(buffer);
        return encore_empty_str();
    }
    return encore_from_owned_buffer(buffer, (size_t)bytes_read);
#else
    ssize_t bytes_read = read(fd, buffer, max_bytes);
    if (bytes_read <= 0) {
        free(buffer);
        return encore_empty_str();
    }
    return encore_from_owned_buffer(buffer, (size_t)bytes_read);
#endif
}

int32_t encore_io_write(int32_t fd, encore_str value) {
    if (fd < 0) {
        return -1;
    }
    size_t len = encore_str_size(value);
    char *data = encore_str_data(value);
    if (len == 0) {
        return 0;
    }
    if (data == NULL) {
        return -1;
    }

    size_t offset = 0;
    while (offset < len) {
#ifdef _WIN32
        size_t remaining = len - offset;
        size_t request = remaining > UINT_MAX ? UINT_MAX : remaining;
        int written = _write(fd, data + offset, (unsigned int)request);
        if (written <= 0) {
            return -1;
        }
        offset += (size_t)written;
#else
        ssize_t written = write(fd, data + offset, len - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (written == 0) {
            return -1;
        }
        offset += (size_t)written;
#endif
    }

    if (fd == 1) {
        fflush(stdout);
    } else if (fd == 2) {
        fflush(stderr);
    }
    return 0;
}

static char g_net_last_error[256] = {0};

static void encore_set_net_error_cstr(const char *msg) {
    if (msg == NULL) {
        g_net_last_error[0] = '\0';
        return;
    }
    snprintf(g_net_last_error, sizeof(g_net_last_error), "%s", msg);
}

static void encore_set_net_error_code(const char *prefix, int code) {
    if (prefix == NULL) {
        prefix = "net";
    }
#ifdef _WIN32
    snprintf(g_net_last_error, sizeof(g_net_last_error), "%s: %d", prefix, code);
#else
    snprintf(g_net_last_error, sizeof(g_net_last_error), "%s: %s", prefix, strerror(code));
#endif
}

encore_str encore_net_last_error(void) {
    if (g_net_last_error[0] == '\0') {
        return encore_empty_str();
    }
    return encore_from_cstr_copy(g_net_last_error);
}

#ifdef _WIN32
static bool g_winsock_initialized = false;

static bool encore_net_init(void) {
    if (g_winsock_initialized) {
        return true;
    }
    WSADATA wsa_data;
    int rc = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (rc != 0) {
        encore_set_net_error_code("WSAStartup failed", rc);
        return false;
    }
    g_winsock_initialized = true;
    return true;
}

static int encore_last_socket_error(void) {
    return WSAGetLastError();
}

static int encore_close_socket(SOCKET fd) {
    return closesocket(fd);
}
#else
static bool encore_net_init(void) {
    return true;
}

static int encore_last_socket_error(void) {
    return errno;
}

static int encore_close_socket(int fd) {
    return close(fd);
}
#endif

static int32_t encore_parse_port(const char *port_c) {
    if (port_c == NULL) {
        return -1;
    }
    char *end = NULL;
    long parsed = strtol(port_c, &end, 10);
    bool ok = end != NULL && *end == '\0' && parsed >= 0 && parsed <= 65535;
    if (!ok) {
        return -1;
    }
    return (int32_t)parsed;
}

int32_t encore_net_tcp_connect(encore_str addr) {
    if (!encore_net_init()) {
        return -1;
    }

    char *addr_c = encore_to_cstr(addr);
    if (addr_c == NULL) {
        encore_set_net_error_cstr("alloc failed");
        return -1;
    }

    char *colon = strrchr(addr_c, ':');
    if (colon == NULL) {
        free(addr_c);
        encore_set_net_error_cstr("invalid addr, expected host:port");
        return -1;
    }
    *colon = '\0';
    const char *host = addr_c;
    int32_t port = encore_parse_port(colon + 1);
    if (port < 0) {
        free(addr_c);
        encore_set_net_error_cstr("invalid port");
        return -1;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_buf[16];
    snprintf(port_buf, sizeof(port_buf), "%d", (int)port);
    struct addrinfo *results = NULL;
    int gai_rc = getaddrinfo(host, port_buf, &hints, &results);
    if (gai_rc != 0 || results == NULL) {
        free(addr_c);
        encore_set_net_error_cstr("getaddrinfo failed");
        return -1;
    }

    int32_t out_fd = -1;
    for (struct addrinfo *it = results; it != NULL; it = it->ai_next) {
#ifdef _WIN32
        SOCKET fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd == INVALID_SOCKET) {
            continue;
        }
        if (connect(fd, it->ai_addr, (int)it->ai_addrlen) == 0) {
            out_fd = (int32_t)fd;
            break;
        }
        encore_close_socket(fd);
#else
        int fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
            out_fd = (int32_t)fd;
            break;
        }
        encore_close_socket(fd);
#endif
    }
    if (out_fd < 0) {
        encore_set_net_error_code("connect failed", encore_last_socket_error());
    }

    freeaddrinfo(results);
    free(addr_c);
    return out_fd;
}

int32_t encore_net_tcp_bind(encore_str addr) {
    if (!encore_net_init()) {
        return -1;
    }

    char *addr_c = encore_to_cstr(addr);
    if (addr_c == NULL) {
        encore_set_net_error_cstr("alloc failed");
        return -1;
    }
    char *colon = strrchr(addr_c, ':');
    if (colon == NULL) {
        free(addr_c);
        encore_set_net_error_cstr("invalid addr, expected host:port");
        return -1;
    }
    *colon = '\0';
    const char *host = addr_c;
    int32_t port = encore_parse_port(colon + 1);
    if (port < 0) {
        free(addr_c);
        encore_set_net_error_cstr("invalid port");
        return -1;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    char port_buf[16];
    snprintf(port_buf, sizeof(port_buf), "%d", (int)port);
    struct addrinfo *results = NULL;
    int gai_rc = getaddrinfo(host, port_buf, &hints, &results);
    if (gai_rc != 0 || results == NULL) {
        free(addr_c);
        encore_set_net_error_cstr("getaddrinfo failed");
        return -1;
    }

    int32_t out_fd = -1;
    for (struct addrinfo *it = results; it != NULL; it = it->ai_next) {
#ifdef _WIN32
        SOCKET fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd == INVALID_SOCKET) {
            continue;
        }
        BOOL reuse = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
        if (bind(fd, it->ai_addr, (int)it->ai_addrlen) == 0 && listen(fd, 64) == 0) {
            out_fd = (int32_t)fd;
            break;
        }
        encore_close_socket(fd);
#else
        int fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }
        int reuse = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if (bind(fd, it->ai_addr, it->ai_addrlen) == 0 && listen(fd, 64) == 0) {
            out_fd = (int32_t)fd;
            break;
        }
        encore_close_socket(fd);
#endif
    }
    if (out_fd < 0) {
        encore_set_net_error_code("bind/listen failed", encore_last_socket_error());
    }

    freeaddrinfo(results);
    free(addr_c);
    return out_fd;
}

int32_t encore_net_tcp_accept(int32_t listener_fd) {
    if (!encore_net_init()) {
        return -1;
    }
#ifdef _WIN32
    SOCKET fd = accept((SOCKET)listener_fd, NULL, NULL);
    if (fd == INVALID_SOCKET) {
        encore_set_net_error_code("accept failed", encore_last_socket_error());
        return -1;
    }
    return (int32_t)fd;
#else
    int fd = accept(listener_fd, NULL, NULL);
    if (fd < 0) {
        encore_set_net_error_code("accept failed", errno);
        return -1;
    }
    return fd;
#endif
}

encore_str encore_net_tcp_read(int32_t fd, size_t max) {
    if (max == 0) {
        return encore_empty_str();
    }
    char *buffer = malloc(max + 1);
    if (buffer == NULL) {
        encore_set_net_error_cstr("alloc failed");
        return encore_empty_str();
    }
#ifdef _WIN32
    int n = recv((SOCKET)fd, buffer, (int)max, 0);
    if (n < 0) {
        free(buffer);
        encore_set_net_error_code("recv failed", encore_last_socket_error());
        return encore_empty_str();
    }
    return encore_from_owned_buffer(buffer, (size_t)n);
#else
    ssize_t n = recv(fd, buffer, max, 0);
    if (n < 0) {
        free(buffer);
        encore_set_net_error_code("recv failed", errno);
        return encore_empty_str();
    }
    return encore_from_owned_buffer(buffer, (size_t)n);
#endif
}

int32_t encore_net_tcp_write(int32_t fd, encore_str data) {
    size_t len = encore_str_size(data);
    char *bytes = encore_str_data(data);
    if (bytes == NULL && len > 0) {
        encore_set_net_error_cstr("invalid data");
        return -1;
    }
#ifdef _WIN32
    int n = send((SOCKET)fd, bytes, (int)len, 0);
    if (n < 0) {
        encore_set_net_error_code("send failed", encore_last_socket_error());
        return -1;
    }
    return n;
#else
    ssize_t n = send(fd, bytes, len, 0);
    if (n < 0) {
        encore_set_net_error_code("send failed", errno);
        return -1;
    }
    return (int32_t)n;
#endif
}

int32_t encore_net_tcp_close(int32_t fd) {
    int rc = encore_close_socket(
#ifdef _WIN32
        (SOCKET)fd
#else
        fd
#endif
    );
    if (rc != 0) {
        encore_set_net_error_code("close failed", encore_last_socket_error());
        return -1;
    }
    return 0;
}

#if defined(__APPLE__)
typedef struct {
    int fd;
    SSLContextRef context;
    bool read_failed;
    bool received_data;
} encore_tls_client;

static int encore_apple_tls_socket(const char *host, size_t port, size_t timeout_ms) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_buf[16];
    snprintf(port_buf, sizeof(port_buf), "%zu", port);
    struct addrinfo *results = NULL;
    if (getaddrinfo(host, port_buf, &hints, &results) != 0 || results == NULL) return -1;
    int connected = -1;
    for (struct addrinfo *it = results; it != NULL; it = it->ai_next) {
        int fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) continue;
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int result = connect(fd, it->ai_addr, it->ai_addrlen);
        if (result == 0) connected = fd;
        else if (errno == EINPROGRESS) {
            struct pollfd pending = {fd, POLLOUT, 0};
            int wait_ms = timeout_ms > 250 ? 250 : (int)timeout_ms;
            if (poll(&pending, 1, wait_ms) > 0) {
                int socket_error = 0;
                socklen_t error_size = sizeof(socket_error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) == 0 && socket_error == 0) connected = fd;
            }
        }
        if (connected >= 0) {
            if (flags >= 0) fcntl(fd, F_SETFL, flags);
            struct timeval timeout = {(time_t)(timeout_ms / 1000), (suseconds_t)((timeout_ms % 1000) * 1000)};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            break;
        }
        close(fd);
    }
    freeaddrinfo(results);
    return connected;
}

static OSStatus encore_apple_tls_read(SSLConnectionRef connection, void *data, size_t *length) {
    int fd = (int)(intptr_t)connection;
    ssize_t count = recv(fd, data, *length, 0);
    if (count > 0) { *length = (size_t)count; return noErr; }
    *length = 0;
    if (count == 0) return errSSLClosedGraceful;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return errSSLWouldBlock;
    return errSSLClosedAbort;
}

static OSStatus encore_apple_tls_write(SSLConnectionRef connection, const void *data, size_t *length) {
    int fd = (int)(intptr_t)connection;
    ssize_t count = send(fd, data, *length, 0);
    if (count >= 0) { *length = (size_t)count; return noErr; }
    *length = 0;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return errSSLWouldBlock;
    return errSSLClosedAbort;
}

static bool encore_apple_add_ca(SecTrustRef trust, const char *path) {
    if (path[0] == '\0') return true;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return false; }
    long file_size = ftell(file);
    if (file_size <= 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return false; }
    UInt8 *bytes = malloc((size_t)file_size);
    if (bytes == NULL || fread(bytes, 1, (size_t)file_size, file) != (size_t)file_size) { free(bytes); fclose(file); return false; }
    fclose(file);
    CFDataRef data = CFDataCreate(NULL, bytes, (CFIndex)file_size);
    free(bytes);
    if (data == NULL) return false;
    SecExternalFormat format = kSecFormatUnknown;
    SecExternalItemType type = kSecItemTypeCertificate;
    CFArrayRef items = NULL;
    OSStatus status = SecItemImport(data, NULL, &format, &type, 0, NULL, NULL, &items);
    CFRelease(data);
    if (status != errSecSuccess || items == NULL || CFArrayGetCount(items) == 0) {
        if (items != NULL) CFRelease(items);
        return false;
    }
    status = SecTrustSetAnchorCertificates(trust, items);
    if (status == errSecSuccess) status = SecTrustSetAnchorCertificatesOnly(trust, false);
    CFRelease(items);
    return status == errSecSuccess;
}

size_t encore_tls_client_connect(encore_str host, size_t port, encore_str ca_file, size_t timeout_ms) {
    char *host_c = encore_to_cstr(host);
    char *ca_c = encore_to_cstr(ca_file);
    if (host_c == NULL || ca_c == NULL || port == 0 || port > 65535 || timeout_ms == 0) {
        free(host_c); free(ca_c); encore_set_net_error_cstr("invalid TLS endpoint"); return 0;
    }
    int fd = encore_apple_tls_socket(host_c, port, timeout_ms);
    if (fd < 0) { free(host_c); free(ca_c); encore_set_net_error_cstr("TLS TCP connect failed"); return 0; }
    SSLContextRef context = SSLCreateContext(NULL, kSSLClientSide, kSSLStreamType);
    if (context == NULL) { close(fd); free(host_c); free(ca_c); encore_set_net_error_cstr("TLS context failed"); return 0; }
    OSStatus status = SSLSetIOFuncs(context, encore_apple_tls_read, encore_apple_tls_write);
    if (status == noErr) status = SSLSetConnection(context, (SSLConnectionRef)(intptr_t)fd);
    if (status == noErr) status = SSLSetPeerDomainName(context, host_c, strlen(host_c));
    if (status == noErr) status = SSLSetProtocolVersionMin(context, kTLSProtocol12);
    if (status == noErr) status = SSLSetSessionOption(context, kSSLSessionOptionBreakOnServerAuth, true);
    if (status == noErr) status = SSLHandshake(context);
    if (status == errSSLServerAuthCompleted) {
        SecTrustRef trust = NULL;
        status = SSLCopyPeerTrust(context, &trust);
        if (status == noErr && !encore_apple_add_ca(trust, ca_c)) status = errSSLXCertChainInvalid;
        if (status == noErr) {
            SecTrustResultType result = kSecTrustResultInvalid;
            status = SecTrustEvaluate(trust, &result);
            if (status == noErr && result != kSecTrustResultProceed && result != kSecTrustResultUnspecified) status = errSSLXCertChainInvalid;
        }
        if (trust != NULL) CFRelease(trust);
        if (status == noErr) status = SSLHandshake(context);
    }
    if (status != noErr) {
        char detail[96]; snprintf(detail, sizeof(detail), "TLS handshake failed (%d)", (int)status);
        encore_set_net_error_cstr(detail); CFRelease(context); close(fd); free(host_c); free(ca_c); return 0;
    }
    encore_tls_client *client = malloc(sizeof(*client));
    if (client == NULL) { encore_set_net_error_cstr("TLS allocation failed"); SSLClose(context); CFRelease(context); close(fd); free(host_c); free(ca_c); return 0; }
    client->fd = fd; client->context = context; client->read_failed = false; client->received_data = false;
    free(host_c); free(ca_c);
    return (size_t)(uintptr_t)client;
}

encore_str encore_tls_read(size_t handle, size_t max) {
    encore_tls_client *client = (encore_tls_client *)(uintptr_t)handle;
    if (client == NULL || max == 0) return encore_empty_str();
    client->read_failed = false;
    char *buffer = malloc(max + 1);
    if (buffer == NULL) { client->read_failed = true; encore_set_net_error_cstr("TLS read allocation failed"); return encore_empty_str(); }
    size_t count = 0;
    OSStatus status = SSLRead(client->context, buffer, max, &count);
    if (count > 0) {
        client->received_data = true;
        return encore_from_owned_buffer(buffer, count);
    }
    free(buffer);
    if (status != errSSLClosedGraceful && status != errSSLClosedNoNotify &&
        !(status == errSSLClosedAbort && client->received_data)) {
        client->read_failed = true;
        encore_set_net_error_cstr(status == errSSLWouldBlock ? "TLS read timed out" : "TLS read failed");
    }
    return encore_empty_str();
}

bool encore_tls_read_failed(size_t handle) {
    encore_tls_client *client = (encore_tls_client *)(uintptr_t)handle;
    return client != NULL && client->read_failed;
}

int32_t encore_tls_write(size_t handle, encore_str data) {
    encore_tls_client *client = (encore_tls_client *)(uintptr_t)handle;
    if (client == NULL) return -1;
    size_t length = encore_str_size(data), offset = 0;
    const char *bytes = encore_str_data(data);
    while (offset < length) {
        size_t written = 0;
        OSStatus status = SSLWrite(client->context, bytes + offset, length - offset, &written);
        offset += written;
        if (status != noErr) { encore_set_net_error_cstr(status == errSSLWouldBlock ? "TLS write timed out" : "TLS write failed"); return -1; }
    }
    return offset > (size_t)INT32_MAX ? INT32_MAX : (int32_t)offset;
}

int32_t encore_tls_close(size_t handle) {
    encore_tls_client *client = (encore_tls_client *)(uintptr_t)handle;
    if (client == NULL) return 0;
    SSLClose(client->context); CFRelease(client->context); close(client->fd); free(client);
    return 0;
}
#elif defined(__linux__) && !defined(__ANDROID__)
typedef struct {
    int fd;
    SSL_CTX *context;
    SSL *ssl;
    bool read_failed;
} encore_tls_client;

static int encore_tls_connect_socket(const char *host, size_t port, size_t timeout_ms) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_buf[16];
    snprintf(port_buf, sizeof(port_buf), "%zu", port);
    struct addrinfo *results = NULL;
    if (getaddrinfo(host, port_buf, &hints, &results) != 0 || results == NULL) return -1;
    int connected = -1;
    for (struct addrinfo *it = results; it != NULL; it = it->ai_next) {
        int fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) continue;
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int result = connect(fd, it->ai_addr, it->ai_addrlen);
        if (result == 0) { connected = fd; }
        else if (errno == EINPROGRESS) {
            struct pollfd pending = {fd, POLLOUT, 0};
            /* Try the next DNS address quickly (Happy-Eyeballs-style fallback)
               instead of stalling on an unreachable IPv6 route. */
            int wait_ms = timeout_ms > 250 ? 250 : (int)timeout_ms;
            if (poll(&pending, 1, wait_ms) > 0) {
                int socket_error = 0;
                socklen_t error_size = sizeof(socket_error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) == 0 && socket_error == 0) connected = fd;
            }
        }
        if (connected >= 0) {
            if (flags >= 0) fcntl(fd, F_SETFL, flags);
            struct timeval timeout = {(time_t)(timeout_ms / 1000), (suseconds_t)((timeout_ms % 1000) * 1000)};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            break;
        }
        close(fd);
    }
    freeaddrinfo(results);
    return connected;
}

static void encore_set_tls_error(const char *prefix) {
    unsigned long code = ERR_get_error();
    if (code == 0) { encore_set_net_error_cstr(prefix); return; }
    char detail[160];
    ERR_error_string_n(code, detail, sizeof(detail));
    snprintf(g_net_last_error, sizeof(g_net_last_error), "%s: %s", prefix, detail);
}

size_t encore_tls_client_connect(encore_str host, size_t port, encore_str ca_file, size_t timeout_ms) {
    char *host_c = encore_to_cstr(host);
    char *ca_c = encore_to_cstr(ca_file);
    if (host_c == NULL || ca_c == NULL || port == 0 || port > 65535 || timeout_ms == 0) {
        free(host_c); free(ca_c); encore_set_net_error_cstr("invalid TLS endpoint"); return 0;
    }
    SSL_CTX *context = SSL_CTX_new(TLS_client_method());
    if (context == NULL) { free(host_c); free(ca_c); encore_set_tls_error("TLS context failed"); return 0; }
    SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION);
#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
    /* HTTP/1.1 connection-close framing is commonly terminated without a TLS
       close-notify. Treat that transport EOF as EOF; certificate and hostname
       verification remain mandatory. */
    SSL_CTX_set_options(context, SSL_OP_IGNORE_UNEXPECTED_EOF);
#endif
    SSL_CTX_set_verify(context, SSL_VERIFY_PEER, NULL);
    int trust_ok = ca_c[0] == '\0' ? SSL_CTX_set_default_verify_paths(context) : SSL_CTX_load_verify_locations(context, ca_c, NULL);
    if (trust_ok != 1) {
        encore_set_tls_error("TLS CA loading failed"); SSL_CTX_free(context); free(host_c); free(ca_c); return 0;
    }
    int fd = encore_tls_connect_socket(host_c, port, timeout_ms);
    if (fd < 0) {
        encore_set_net_error_code("TLS TCP connect failed", errno); SSL_CTX_free(context); free(host_c); free(ca_c); return 0;
    }
    SSL *ssl = SSL_new(context);
    if (ssl == NULL || SSL_set_fd(ssl, fd) != 1 || SSL_set_tlsext_host_name(ssl, host_c) != 1 || SSL_set1_host(ssl, host_c) != 1 || SSL_connect(ssl) != 1) {
        encore_set_tls_error("TLS handshake failed");
        if (ssl != NULL) SSL_free(ssl);
        close(fd); SSL_CTX_free(context); free(host_c); free(ca_c); return 0;
    }
    encore_tls_client *client = malloc(sizeof(*client));
    if (client == NULL) {
        encore_set_net_error_cstr("TLS allocation failed"); SSL_shutdown(ssl); SSL_free(ssl); close(fd); SSL_CTX_free(context); free(host_c); free(ca_c); return 0;
    }
    client->fd = fd; client->context = context; client->ssl = ssl; client->read_failed = false;
    free(host_c); free(ca_c);
    return (size_t)(uintptr_t)client;
}

encore_str encore_tls_read(size_t handle, size_t max) {
    encore_tls_client *client = (encore_tls_client *)(uintptr_t)handle;
    if (client == NULL || max == 0) return encore_empty_str();
    client->read_failed = false;
    if (max > (size_t)INT_MAX) max = INT_MAX;
    char *buffer = malloc(max + 1);
    if (buffer == NULL) { encore_set_net_error_cstr("TLS read allocation failed"); return encore_empty_str(); }
    int count = SSL_read(client->ssl, buffer, (int)max);
    if (count > 0) return encore_from_owned_buffer(buffer, (size_t)count);
    int error = SSL_get_error(client->ssl, count);
    free(buffer);
    if (error != SSL_ERROR_ZERO_RETURN) { client->read_failed = true; encore_set_tls_error("TLS read failed"); }
    return encore_empty_str();
}

bool encore_tls_read_failed(size_t handle) {
    encore_tls_client *client = (encore_tls_client *)(uintptr_t)handle;
    return client != NULL && client->read_failed;
}

int32_t encore_tls_write(size_t handle, encore_str data) {
    encore_tls_client *client = (encore_tls_client *)(uintptr_t)handle;
    if (client == NULL) { encore_set_net_error_cstr("invalid TLS handle"); return -1; }
    size_t length = encore_str_size(data);
    const char *bytes = encore_str_data(data);
    size_t offset = 0;
    while (offset < length) {
        size_t remaining = length - offset;
        int amount = remaining > (size_t)INT_MAX ? INT_MAX : (int)remaining;
        int written = SSL_write(client->ssl, bytes + offset, amount);
        if (written <= 0) { encore_set_tls_error("TLS write failed"); return -1; }
        offset += (size_t)written;
    }
    return offset > (size_t)INT32_MAX ? INT32_MAX : (int32_t)offset;
}

int32_t encore_tls_close(size_t handle) {
    encore_tls_client *client = (encore_tls_client *)(uintptr_t)handle;
    if (client == NULL) return 0;
    SSL_shutdown(client->ssl);
    SSL_free(client->ssl);
    close(client->fd);
    SSL_CTX_free(client->context);
    free(client);
    return 0;
}
#elif defined(_WIN32)
typedef struct {
    SOCKET socket;
    CredHandle credentials;
    CtxtHandle context;
    SecPkgContext_StreamSizes sizes;
    unsigned char encrypted[131072];
    size_t encrypted_len;
    unsigned char *plain;
    size_t plain_len;
    size_t plain_offset;
    bool read_failed;
} encore_tls_client;

static void encore_windows_tls_status(const char *operation, SECURITY_STATUS status) {
    snprintf(g_net_last_error, sizeof(g_net_last_error), "%s (0x%08lx)", operation, (unsigned long)status);
}

static SOCKET encore_windows_tls_socket(const char *host, size_t port, size_t timeout_ms) {
    if (!encore_net_init()) return INVALID_SOCKET;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_buf[16];
    snprintf(port_buf, sizeof(port_buf), "%zu", port);
    struct addrinfo *results = NULL;
    if (getaddrinfo(host, port_buf, &hints, &results) != 0) return INVALID_SOCKET;
    SOCKET connected = INVALID_SOCKET;
    for (struct addrinfo *it = results; it != NULL; it = it->ai_next) {
        SOCKET socket_fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (socket_fd == INVALID_SOCKET) continue;
        u_long nonblocking = 1;
        ioctlsocket(socket_fd, FIONBIO, &nonblocking);
        int result = connect(socket_fd, it->ai_addr, (int)it->ai_addrlen);
        if (result == 0 || WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set writable;
            FD_ZERO(&writable); FD_SET(socket_fd, &writable);
            struct timeval wait = {(long)(timeout_ms / 1000), (long)((timeout_ms % 1000) * 1000)};
            if (result == 0 || select(0, NULL, &writable, NULL, &wait) > 0) {
                int socket_error = 0; int error_size = sizeof(socket_error);
                if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, (char *)&socket_error, &error_size) == 0 && socket_error == 0) connected = socket_fd;
            }
        }
        if (connected != INVALID_SOCKET) {
            nonblocking = 0; ioctlsocket(socket_fd, FIONBIO, &nonblocking);
            DWORD timeout = timeout_ms > UINT32_MAX ? UINT32_MAX : (DWORD)timeout_ms;
            setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
            setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
            break;
        }
        closesocket(socket_fd);
    }
    freeaddrinfo(results);
    return connected;
}

static bool encore_windows_send_all(SOCKET socket_fd, const void *data, size_t length) {
    const char *bytes = data;
    size_t offset = 0;
    while (offset < length) {
        int amount = length - offset > INT_MAX ? INT_MAX : (int)(length - offset);
        int written = send(socket_fd, bytes + offset, amount, 0);
        if (written <= 0) return false;
        offset += (size_t)written;
    }
    return true;
}

static bool encore_windows_validate_certificate(CtxtHandle *context, const wchar_t *host, const char *ca_file) {
    PCCERT_CONTEXT certificate = NULL;
    SECURITY_STATUS status = QueryContextAttributes(context, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &certificate);
    if (status != SEC_E_OK || certificate == NULL) return false;
    HCERTCHAINENGINE engine = HCCE_CURRENT_USER;
    HCERTSTORE store = NULL;
    PCCERT_CONTEXT root = NULL;
    if (ca_file[0] != '\0') {
        FILE *file = fopen(ca_file, "rb");
        if (file == NULL) { CertFreeCertificateContext(certificate); return false; }
        fseek(file, 0, SEEK_END); long file_size = ftell(file); rewind(file);
        char *encoded = file_size > 0 ? malloc((size_t)file_size + 1) : NULL;
        if (encoded == NULL || fread(encoded, 1, (size_t)file_size, file) != (size_t)file_size) { fclose(file); free(encoded); CertFreeCertificateContext(certificate); return false; }
        fclose(file); encoded[file_size] = '\0';
        DWORD der_size = 0;
        bool pem = CryptStringToBinaryA(encoded, (DWORD)file_size, CRYPT_STRING_BASE64HEADER, NULL, &der_size, NULL, NULL) != 0;
        if (!pem) der_size = (DWORD)file_size;
        BYTE *der = malloc(der_size);
        if (der != NULL && pem) pem = CryptStringToBinaryA(encoded, (DWORD)file_size, CRYPT_STRING_BASE64HEADER, der, &der_size, NULL, NULL) != 0;
        else if (der != NULL) memcpy(der, encoded, der_size);
        free(encoded);
        if (der == NULL || (pem == false && der_size == 0)) { free(der); CertFreeCertificateContext(certificate); return false; }
        root = CertCreateCertificateContext(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, der, der_size);
        free(der);
        store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, NULL);
        if (root == NULL || store == NULL || !CertAddCertificateContextToStore(store, root, CERT_STORE_ADD_ALWAYS, NULL)) {
            if (root != NULL) CertFreeCertificateContext(root); if (store != NULL) CertCloseStore(store, 0); CertFreeCertificateContext(certificate); return false;
        }
        CERT_CHAIN_ENGINE_CONFIG config;
        memset(&config, 0, sizeof(config)); config.cbSize = sizeof(config); config.hExclusiveRoot = store;
        if (!CertCreateCertificateChainEngine(&config, &engine)) { CertFreeCertificateContext(root); CertCloseStore(store, 0); CertFreeCertificateContext(certificate); return false; }
    }
    CERT_CHAIN_PARA chain_parameters;
    memset(&chain_parameters, 0, sizeof(chain_parameters)); chain_parameters.cbSize = sizeof(chain_parameters);
    PCCERT_CHAIN_CONTEXT chain = NULL;
    BOOL chain_ok = CertGetCertificateChain(engine, certificate, NULL, certificate->hCertStore, &chain_parameters, 0, NULL, &chain);
    HTTPSPolicyCallbackData policy_data;
    memset(&policy_data, 0, sizeof(policy_data)); policy_data.cbStruct = sizeof(policy_data); policy_data.dwAuthType = AUTHTYPE_SERVER; policy_data.pwszServerName = (wchar_t *)host;
    CERT_CHAIN_POLICY_PARA policy_parameters;
    memset(&policy_parameters, 0, sizeof(policy_parameters)); policy_parameters.cbSize = sizeof(policy_parameters); policy_parameters.pvExtraPolicyPara = &policy_data;
    CERT_CHAIN_POLICY_STATUS policy_status;
    memset(&policy_status, 0, sizeof(policy_status)); policy_status.cbSize = sizeof(policy_status);
    BOOL policy_ok = chain_ok && CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy_parameters, &policy_status) && policy_status.dwError == 0;
    if (chain != NULL) CertFreeCertificateChain(chain);
    if (ca_file[0] != '\0') { CertFreeCertificateChainEngine(engine); CertFreeCertificateContext(root); CertCloseStore(store, 0); }
    CertFreeCertificateContext(certificate);
    return policy_ok != 0;
}

size_t encore_tls_client_connect(encore_str host, size_t port, encore_str ca_file, size_t timeout_ms) {
    char *host_c = encore_to_cstr(host), *ca_c = encore_to_cstr(ca_file);
    if (host_c == NULL || ca_c == NULL || port == 0 || port > 65535 || timeout_ms == 0) { free(host_c); free(ca_c); encore_set_net_error_cstr("invalid TLS endpoint"); return 0; }
    int wide_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, host_c, -1, NULL, 0);
    wchar_t *host_w = wide_len > 0 ? malloc((size_t)wide_len * sizeof(wchar_t)) : NULL;
    if (host_w == NULL || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, host_c, -1, host_w, wide_len) == 0) { free(host_w); free(host_c); free(ca_c); encore_set_net_error_cstr("invalid TLS host"); return 0; }
    SOCKET socket_fd = encore_windows_tls_socket(host_c, port, timeout_ms);
    if (socket_fd == INVALID_SOCKET) { free(host_w); free(host_c); free(ca_c); encore_set_net_error_cstr("TLS TCP connect failed"); return 0; }
    SCHANNEL_CRED credential_data;
    memset(&credential_data, 0, sizeof(credential_data)); credential_data.dwVersion = SCHANNEL_CRED_VERSION;
    credential_data.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
#ifdef SP_PROT_TLS1_3_CLIENT
    credential_data.grbitEnabledProtocols |= SP_PROT_TLS1_3_CLIENT;
#endif
    credential_data.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
    CredHandle credentials; TimeStamp expiry;
    SECURITY_STATUS status = AcquireCredentialsHandleW(NULL, UNISP_NAME_W, SECPKG_CRED_OUTBOUND, NULL, &credential_data, NULL, NULL, &credentials, &expiry);
    if (status != SEC_E_OK) { encore_windows_tls_status("TLS credentials failed", status); closesocket(socket_fd); free(host_w); free(host_c); free(ca_c); return 0; }
    DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
    DWORD attributes = 0;
    CtxtHandle context;
    SecInvalidateHandle(&context);
    SecBuffer output_buffer = {0, SECBUFFER_TOKEN, NULL}; SecBufferDesc output = {SECBUFFER_VERSION, 1, &output_buffer};
    status = InitializeSecurityContextW(&credentials, NULL, host_w, flags, 0, SECURITY_NATIVE_DREP, NULL, 0, &context, &output, &attributes, &expiry);
    if (status == SEC_I_COMPLETE_NEEDED || status == SEC_I_COMPLETE_AND_CONTINUE) {
        SECURITY_STATUS completed = CompleteAuthToken(&context, &output);
        if (completed != SEC_E_OK) status = completed;
        else status = status == SEC_I_COMPLETE_NEEDED ? SEC_E_OK : SEC_I_CONTINUE_NEEDED;
    }
    if (output_buffer.pvBuffer != NULL) { if (!encore_windows_send_all(socket_fd, output_buffer.pvBuffer, output_buffer.cbBuffer)) status = SEC_E_INTERNAL_ERROR; FreeContextBuffer(output_buffer.pvBuffer); }
    unsigned char incoming[131072]; size_t incoming_len = 0;
    while (status == SEC_I_CONTINUE_NEEDED || status == SEC_E_INCOMPLETE_MESSAGE) {
        if (incoming_len == sizeof(incoming)) { status = SEC_E_BUFFER_TOO_SMALL; break; }
        int received = recv(socket_fd, (char *)incoming + incoming_len, (int)(sizeof(incoming) - incoming_len), 0);
        if (received <= 0) { status = SEC_E_INTERNAL_ERROR; break; }
        incoming_len += (size_t)received;
        SecBuffer input_buffers[2] = {{(ULONG)incoming_len, SECBUFFER_TOKEN, incoming}, {0, SECBUFFER_EMPTY, NULL}};
        SecBufferDesc input = {SECBUFFER_VERSION, 2, input_buffers};
        output_buffer.cbBuffer = 0; output_buffer.BufferType = SECBUFFER_TOKEN; output_buffer.pvBuffer = NULL;
        status = InitializeSecurityContextW(&credentials, &context, host_w, flags, 0, SECURITY_NATIVE_DREP, &input, 0, NULL, &output, &attributes, &expiry);
        if (status == SEC_I_COMPLETE_NEEDED || status == SEC_I_COMPLETE_AND_CONTINUE) {
            SECURITY_STATUS completed = CompleteAuthToken(&context, &output);
            if (completed != SEC_E_OK) status = completed;
            else status = status == SEC_I_COMPLETE_NEEDED ? SEC_E_OK : SEC_I_CONTINUE_NEEDED;
        }
        if (output_buffer.pvBuffer != NULL) { if (!encore_windows_send_all(socket_fd, output_buffer.pvBuffer, output_buffer.cbBuffer)) status = SEC_E_INTERNAL_ERROR; FreeContextBuffer(output_buffer.pvBuffer); }
        if (status != SEC_E_INCOMPLETE_MESSAGE) {
            if (input_buffers[1].BufferType == SECBUFFER_EXTRA) { incoming_len = input_buffers[1].cbBuffer; memmove(incoming, input_buffers[1].pvBuffer, incoming_len); }
            else incoming_len = 0;
        }
    }
    if (status != SEC_E_OK || !encore_windows_validate_certificate(&context, host_w, ca_c)) {
        if (status != SEC_E_OK) encore_windows_tls_status("TLS handshake failed", status); else encore_set_net_error_cstr("TLS certificate or hostname verification failed");
        if (SecIsValidHandle(&context)) DeleteSecurityContext(&context); FreeCredentialsHandle(&credentials); closesocket(socket_fd); free(host_w); free(host_c); free(ca_c); return 0;
    }
    encore_tls_client *client = calloc(1, sizeof(*client));
    if (client == NULL || QueryContextAttributes(&context, SECPKG_ATTR_STREAM_SIZES, client != NULL ? &client->sizes : NULL) != SEC_E_OK) {
        free(client); if (SecIsValidHandle(&context)) DeleteSecurityContext(&context); FreeCredentialsHandle(&credentials); closesocket(socket_fd); free(host_w); free(host_c); free(ca_c); encore_set_net_error_cstr("TLS stream setup failed"); return 0;
    }
    client->socket = socket_fd; client->credentials = credentials; client->context = context; client->encrypted_len = incoming_len; memcpy(client->encrypted, incoming, incoming_len);
    free(host_w); free(host_c); free(ca_c); return (size_t)(uintptr_t)client;
}

encore_str encore_tls_read(size_t handle, size_t max) {
    encore_tls_client *client = (encore_tls_client *)(uintptr_t)handle;
    if (client == NULL || max == 0) return encore_empty_str();
    client->read_failed = false;
    if (client->plain_offset < client->plain_len) {
        size_t count = client->plain_len - client->plain_offset; if (count > max) count = max;
        char *out = malloc(count + 1); if (out == NULL) { client->read_failed = true; return encore_empty_str(); }
        memcpy(out, client->plain + client->plain_offset, count); client->plain_offset += count;
        if (client->plain_offset == client->plain_len) { free(client->plain); client->plain = NULL; client->plain_len = client->plain_offset = 0; }
        return encore_from_owned_buffer(out, count);
    }
    for (;;) {
        if (client->encrypted_len == 0) {
            int received = recv(client->socket, (char *)client->encrypted, (int)sizeof(client->encrypted), 0);
            if (received == 0) return encore_empty_str();
            if (received < 0) { client->read_failed = true; encore_set_net_error_cstr(WSAGetLastError() == WSAETIMEDOUT ? "TLS read timed out" : "TLS read failed"); return encore_empty_str(); }
            client->encrypted_len = (size_t)received;
        }
        SecBuffer buffers[4] = {{(ULONG)client->encrypted_len, SECBUFFER_DATA, client->encrypted}, {0, SECBUFFER_EMPTY, NULL}, {0, SECBUFFER_EMPTY, NULL}, {0, SECBUFFER_EMPTY, NULL}};
        SecBufferDesc message = {SECBUFFER_VERSION, 4, buffers};
        SECURITY_STATUS status = DecryptMessage(&client->context, &message, 0, NULL);
        if (status == SEC_E_INCOMPLETE_MESSAGE) {
            if (client->encrypted_len == sizeof(client->encrypted)) {
                client->read_failed = true;
                encore_set_net_error_cstr("TLS record exceeds buffer");
                return encore_empty_str();
            }
            int received = recv(client->socket, (char *)client->encrypted + client->encrypted_len, (int)(sizeof(client->encrypted) - client->encrypted_len), 0);
            if (received <= 0) { client->read_failed = true; encore_set_net_error_cstr("TLS read failed"); return encore_empty_str(); }
            client->encrypted_len += (size_t)received; continue;
        }
        if (status == SEC_I_CONTEXT_EXPIRED) { client->encrypted_len = 0; return encore_empty_str(); }
        if (status != SEC_E_OK && status != SEC_I_RENEGOTIATE) { client->read_failed = true; encore_windows_tls_status("TLS decrypt failed", status); return encore_empty_str(); }
        SecBuffer *data = NULL, *extra = NULL;
        for (size_t index = 1; index < 4; index++) { if (buffers[index].BufferType == SECBUFFER_DATA) data = &buffers[index]; else if (buffers[index].BufferType == SECBUFFER_EXTRA) extra = &buffers[index]; }
        unsigned char *plain = data != NULL && data->cbBuffer > 0 ? malloc(data->cbBuffer) : NULL;
        if (data != NULL && data->cbBuffer > 0 && plain == NULL) { client->read_failed = true; return encore_empty_str(); }
        if (plain != NULL) memcpy(plain, data->pvBuffer, data->cbBuffer);
        if (extra != NULL) { memmove(client->encrypted, extra->pvBuffer, extra->cbBuffer); client->encrypted_len = extra->cbBuffer; } else client->encrypted_len = 0;
        if (plain != NULL) { client->plain = plain; client->plain_len = data->cbBuffer; client->plain_offset = 0; return encore_tls_read(handle, max); }
        if (status == SEC_I_RENEGOTIATE) { client->read_failed = true; encore_set_net_error_cstr("TLS renegotiation is not supported"); return encore_empty_str(); }
    }
}

bool encore_tls_read_failed(size_t handle) { encore_tls_client *client = (encore_tls_client *)(uintptr_t)handle; return client != NULL && client->read_failed; }

int32_t encore_tls_write(size_t handle, encore_str data) {
    encore_tls_client *client = (encore_tls_client *)(uintptr_t)handle;
    if (client == NULL) return -1;
    const unsigned char *bytes = (const unsigned char *)encore_str_data(data); size_t length = encore_str_size(data), offset = 0;
    size_t record_capacity = client->sizes.cbHeader + client->sizes.cbMaximumMessage + client->sizes.cbTrailer;
    unsigned char *record = malloc(record_capacity); if (record == NULL) return -1;
    while (offset < length) {
        size_t count = length - offset; if (count > client->sizes.cbMaximumMessage) count = client->sizes.cbMaximumMessage;
        memcpy(record + client->sizes.cbHeader, bytes + offset, count);
        SecBuffer buffers[4] = {{client->sizes.cbHeader, SECBUFFER_STREAM_HEADER, record}, {(ULONG)count, SECBUFFER_DATA, record + client->sizes.cbHeader}, {client->sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, record + client->sizes.cbHeader + count}, {0, SECBUFFER_EMPTY, NULL}};
        SecBufferDesc message = {SECBUFFER_VERSION, 4, buffers}; SECURITY_STATUS status = EncryptMessage(&client->context, 0, &message, 0);
        if (status != SEC_E_OK || !encore_windows_send_all(client->socket, record, buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer)) { free(record); encore_set_net_error_cstr("TLS write failed"); return -1; }
        offset += count;
    }
    free(record); return offset > INT32_MAX ? INT32_MAX : (int32_t)offset;
}

int32_t encore_tls_close(size_t handle) {
    encore_tls_client *client = (encore_tls_client *)(uintptr_t)handle; if (client == NULL) return 0;
    DWORD shutdown = SCHANNEL_SHUTDOWN; SecBuffer buffer = {sizeof(shutdown), SECBUFFER_TOKEN, &shutdown}; SecBufferDesc message = {SECBUFFER_VERSION, 1, &buffer}; ApplyControlToken(&client->context, &message);
    SecBuffer output_buffer = {0, SECBUFFER_TOKEN, NULL}; SecBufferDesc output = {SECBUFFER_VERSION, 1, &output_buffer}; DWORD attributes = 0; TimeStamp expiry;
    SECURITY_STATUS status = InitializeSecurityContextW(&client->credentials, &client->context, NULL, ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM, 0, SECURITY_NATIVE_DREP, NULL, 0, NULL, &output, &attributes, &expiry);
    if ((status == SEC_E_OK || status == SEC_I_CONTEXT_EXPIRED) && output_buffer.pvBuffer != NULL) encore_windows_send_all(client->socket, output_buffer.pvBuffer, output_buffer.cbBuffer);
    if (output_buffer.pvBuffer != NULL) FreeContextBuffer(output_buffer.pvBuffer);
    DeleteSecurityContext(&client->context); FreeCredentialsHandle(&client->credentials); closesocket(client->socket); free(client->plain); free(client); return 0;
}
#else
size_t encore_tls_client_connect(encore_str host, size_t port, encore_str ca_file, size_t timeout_ms) {
    (void)host; (void)port; (void)ca_file; (void)timeout_ms;
    encore_set_net_error_cstr("system TLS backend is not implemented on this platform");
    return 0;
}
encore_str encore_tls_read(size_t handle, size_t max) { (void)handle; (void)max; return encore_empty_str(); }
bool encore_tls_read_failed(size_t handle) { (void)handle; return true; }
int32_t encore_tls_write(size_t handle, encore_str data) { (void)handle; (void)data; return -1; }
int32_t encore_tls_close(size_t handle) { (void)handle; return 0; }
#endif

int32_t encore_proc_exit(int32_t code) {
    /* The Encore standard library also exports a function named `exit`.
       Calling libc's exit here can therefore bind back to the generated
       symbol and recurse forever. `_Exit` has no generated-name collision. */
    _Exit(code);
    return code;
}

int32_t encore_proc_run(encore_str command) {
    char *command_c = encore_to_cstr(command);
    if (command_c == NULL) {
        return -1;
    }

    int rc = system(command_c);
    free(command_c);
    if (rc == -1) {
        return -1;
    }
#ifdef _WIN32
    return rc;
#else
    if (WIFEXITED(rc)) {
        return WEXITSTATUS(rc);
    }
    if (WIFSIGNALED(rc)) {
        return 128 + WTERMSIG(rc);
    }
    return rc;
#endif
}

static int32_t encore_proc_run_args_impl(encore_str program, size_t raw_args, size_t args_len, const char *output_path) {
    encore_str *args = (encore_str *)(uintptr_t)raw_args;
    char *program_c = encore_to_cstr(program);
    if (program_c == NULL) return -1;
    char **argv = calloc(args_len + 2, sizeof(char *));
    if (argv == NULL) { free(program_c); return -1; }
    argv[0] = program_c;
    size_t converted = 0;
    for (; converted < args_len; converted += 1) {
        argv[converted + 1] = encore_to_cstr(args[converted]);
        if (argv[converted + 1] == NULL) break;
    }
    if (converted != args_len) {
        for (size_t index = 0; index <= converted; index += 1) free(argv[index]);
        free(argv);
        return -1;
    }

    int32_t result = -1;
#ifdef _WIN32
    int saved_stdout = -1;
    int saved_stderr = -1;
    int output_fd = -1;
    if (output_path != NULL) {
        output_fd = _open(output_path, _O_CREAT | _O_TRUNC | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
        if (output_fd < 0) goto cleanup;
        saved_stdout = _dup(1);
        saved_stderr = _dup(2);
        if (saved_stdout < 0 || saved_stderr < 0 || _dup2(output_fd, 1) != 0 || _dup2(output_fd, 2) != 0) goto cleanup;
    }
    intptr_t status = _spawnvp(_P_WAIT, program_c, (const char *const *)argv);
    if (status >= 0 && status <= INT32_MAX) result = (int32_t)status;
cleanup:
    if (saved_stdout >= 0) { fflush(stdout); _dup2(saved_stdout, 1); _close(saved_stdout); }
    if (saved_stderr >= 0) { fflush(stderr); _dup2(saved_stderr, 2); _close(saved_stderr); }
    if (output_fd >= 0) _close(output_fd);
#else
    posix_spawn_file_actions_t actions;
    bool actions_initialized = posix_spawn_file_actions_init(&actions) == 0;
    bool actions_ready = actions_initialized;
    int output_fd = -1;
    if (actions_ready && output_path != NULL) {
        output_fd = open(output_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (output_fd < 0 || posix_spawn_file_actions_adddup2(&actions, output_fd, STDOUT_FILENO) != 0 ||
            posix_spawn_file_actions_adddup2(&actions, output_fd, STDERR_FILENO) != 0 ||
            posix_spawn_file_actions_addclose(&actions, output_fd) != 0) actions_ready = false;
    }
    pid_t child = -1;
#ifdef __APPLE__
    char **envp = *_NSGetEnviron();
#else
    char **envp = environ;
#endif
    int spawn_status = actions_ready ? posix_spawnp(&child, program_c, &actions, NULL, argv, envp) : EINVAL;
    if (output_fd >= 0) close(output_fd);
    if (actions_initialized) posix_spawn_file_actions_destroy(&actions);
    if (spawn_status == 0 && child > 0) {
        int status = 0;
        if (waitpid(child, &status, 0) >= 0) {
            if (WIFEXITED(status)) result = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) result = 128 + WTERMSIG(status);
        }
    }
#endif
    for (size_t index = 0; index < args_len + 1; index += 1) free(argv[index]);
    free(argv);
    return result;
}

int32_t encore_proc_run_args_parts(encore_str program, size_t raw_args, size_t args_len) {
    return encore_proc_run_args_impl(program, raw_args, args_len, NULL);
}

int32_t encore_proc_run_args_capture_parts(encore_str program, size_t raw_args, size_t args_len, encore_str output_path) {
    char *output_c = encore_to_cstr(output_path);
    if (output_c == NULL) return -1;
    int32_t result = encore_proc_run_args_impl(program, raw_args, args_len, output_c);
    free(output_c);
    return result;
}

/* Compatibility with bootstrap objects emitted before Vec extern arguments
 * were lowered to scalar ABI parts. LLVM passes this aggregate in registers. */
int32_t encore_proc_run_args(encore_str program, encore_str *args, size_t len, size_t cap) {
    (void)cap;
    return encore_proc_run_args_parts(program, (size_t)(uintptr_t)args, len);
}

static bool g_args_initialized = false;
static size_t g_argc = 0;
static char **g_argv = NULL;

static void encore_free_args(void) {
    if (g_argv == NULL) {
        return;
    }
    for (size_t i = 0; i < g_argc; ++i) {
        free(g_argv[i]);
    }
    free(g_argv);
    g_argv = NULL;
    g_argc = 0;
}

static void encore_init_args(void) {
    if (g_args_initialized) {
        return;
    }
    g_args_initialized = true;
    atexit(encore_free_args);

#ifdef _WIN32
#ifdef __MINGW32__
    int argc = __argc;
    char **argv = __argv;
#else
    int argc = *__p___argc();
    char **argv = *__p___argv();
#endif
    if (argc <= 0 || argv == NULL) {
        return;
    }
    g_argc = (size_t)argc;
    g_argv = calloc(g_argc, sizeof(char *));
    if (g_argv == NULL) {
        g_argc = 0;
        return;
    }
    for (size_t i = 0; i < g_argc; ++i) {
        if (argv[i] != NULL) {
            size_t len = strlen(argv[i]);
            g_argv[i] = malloc(len + 1);
            if (g_argv[i] != NULL) {
                memcpy(g_argv[i], argv[i], len + 1);
            }
        }
    }
    return;
#endif

#ifdef __APPLE__
    int argc = *_NSGetArgc();
    char **argv = *_NSGetArgv();
    if (argc <= 0 || argv == NULL) {
        return;
    }
    g_argc = (size_t)argc;
    g_argv = calloc(g_argc, sizeof(char *));
    if (g_argv == NULL) {
        g_argc = 0;
        return;
    }
    for (size_t i = 0; i < g_argc; ++i) {
        if (argv[i] != NULL) {
            g_argv[i] = strdup(argv[i]);
        }
    }
    return;
#endif

    FILE *file = fopen("/proc/self/cmdline", "rb");
    if (file == NULL) {
        return;
    }
    size_t capacity = 256;
    size_t read_count = 0;
    char *buffer = malloc(capacity);
    if (buffer == NULL) {
        fclose(file);
        return;
    }

    while (true) {
        if (read_count == capacity) {
            size_t next_capacity = capacity * 2;
            char *next_buffer = realloc(buffer, next_capacity);
            if (next_buffer == NULL) {
                free(buffer);
                fclose(file);
                return;
            }
            buffer = next_buffer;
            capacity = next_capacity;
        }

        size_t chunk = fread(buffer + read_count, 1, capacity - read_count, file);
        read_count += chunk;
        if (chunk == 0) {
            break;
        }
    }
    fclose(file);
    if (read_count == 0) {
        free(buffer);
        return;
    }

    for (size_t i = 0; i < read_count; ++i) {
        if (buffer[i] == '\0') {
            g_argc += 1;
        }
    }
    if (g_argc == 0) {
        free(buffer);
        return;
    }

    g_argv = calloc(g_argc, sizeof(char *));
    if (g_argv == NULL) {
        g_argc = 0;
        free(buffer);
        return;
    }

    size_t arg_index = 0;
    size_t start = 0;
    for (size_t i = 0; i < read_count; ++i) {
        if (buffer[i] != '\0') {
            continue;
        }

        size_t len = i - start;
        char *arg = malloc(len + 1);
        if (arg == NULL) {
            start = i + 1;
            continue;
        }
        memcpy(arg, buffer + start, len);
        arg[len] = '\0';
        g_argv[arg_index++] = arg;
        start = i + 1;
    }

    free(buffer);
}

size_t encore_os_argc(void) {
    encore_init_args();
    return g_argc;
}

encore_str encore_os_argv(size_t index) {
    encore_init_args();
    if (index >= g_argc || g_argv == NULL || g_argv[index] == NULL) {
        return encore_empty_str();
    }
    return encore_from_cstr_copy(g_argv[index]);
}

encore_str encore_os_cwd(void) {
    size_t size = 256;

    for (;;) {
        char *buffer = malloc(size);
        if (buffer == NULL) {
            return encore_empty_str();
        }

        char *cwd_result =
#ifdef _WIN32
            _getcwd(buffer, (int)size);
#else
            getcwd(buffer, size);
#endif
        if (cwd_result != NULL) {
            size_t len = strlen(buffer);
            return encore_from_owned_buffer(buffer, len);
        }

        free(buffer);
        if (errno != ERANGE) {
            return encore_empty_str();
        }

        if (size > (SIZE_MAX / 2)) {
            return encore_empty_str();
        }
        size *= 2;
    }
}

encore_str encore_os_home_dir(void) {
#ifdef _WIN32
    const char *home = getenv("USERPROFILE");
    if (home == NULL || home[0] == '\0') {
        const char *drive = getenv("HOMEDRIVE");
        const char *path = getenv("HOMEPATH");
        if (drive != NULL && path != NULL) {
            size_t drive_len = strlen(drive);
            size_t path_len = strlen(path);
            char *buffer = malloc(drive_len + path_len + 1);
            if (buffer == NULL) {
                return encore_empty_str();
            }
            memcpy(buffer, drive, drive_len);
            memcpy(buffer + drive_len, path, path_len + 1);
            return encore_from_owned_buffer(buffer, drive_len + path_len);
        }
    }
#else
    const char *home = getenv("HOME");
#endif
    if (home == NULL || home[0] == '\0') {
        return encore_empty_str();
    }
    return encore_from_cstr_copy(home);
}

encore_str encore_os_arch(void) {
#ifdef _WIN32
    SYSTEM_INFO info;
    GetNativeSystemInfo(&info);
    switch (info.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: return encore_from_cstr_copy("x86_64");
        case PROCESSOR_ARCHITECTURE_ARM64: return encore_from_cstr_copy("aarch64");
        case PROCESSOR_ARCHITECTURE_INTEL: return encore_from_cstr_copy("i686");
        case PROCESSOR_ARCHITECTURE_ARM: return encore_from_cstr_copy("arm");
        default: return encore_from_cstr_copy("unknown");
    }
#else
    struct utsname info;
    if (uname(&info) != 0) return encore_from_cstr_copy("unknown");
    if (strcmp(info.machine, "amd64") == 0) return encore_from_cstr_copy("x86_64");
    if (strcmp(info.machine, "arm64") == 0) return encore_from_cstr_copy("aarch64");
    return encore_from_cstr_copy(info.machine);
#endif
}

encore_str encore_os_getenv(encore_str name) {
    char *name_c = encore_to_cstr(name);
    if (name_c == NULL) return encore_empty_str();
    const char *value = getenv(name_c);
    free(name_c);
    if (value == NULL || value[0] == '\0') return encore_empty_str();
    return encore_from_cstr_copy(value);
}

encore_str encore_os_executable_path(void) {
    char executable[PATH_MAX];
#ifdef _WIN32
    DWORD written = GetModuleFileNameA(NULL, executable, (DWORD)sizeof(executable));
    if (written == 0 || written >= sizeof(executable)) return encore_empty_str();
    return encore_from_cstr_copy(executable);
#elif defined(__APPLE__)
    uint32_t capacity = (uint32_t)sizeof(executable);
    if (_NSGetExecutablePath(executable, &capacity) != 0) return encore_empty_str();
    char resolved[PATH_MAX];
    if (realpath(executable, resolved) != NULL) return encore_from_cstr_copy(resolved);
    return encore_from_cstr_copy(executable);
#else
    ssize_t written = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (written <= 0) return encore_empty_str();
    executable[written] = '\0';
    return encore_from_cstr_copy(executable);
#endif
}

encore_str encore_fs_read_file(encore_str path) {
    char *path_c = encore_to_cstr(path);
    if (path_c == NULL) {
        return encore_empty_str();
    }

    FILE *file = fopen(path_c, "rb");
    free(path_c);
    if (file == NULL) {
        return encore_empty_str();
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return encore_empty_str();
    }
    long length = ftell(file);
    if (length < 0) {
        fclose(file);
        return encore_empty_str();
    }
    rewind(file);

    char *buffer = malloc((size_t)length + 1);
    if (buffer == NULL) {
        fclose(file);
        return encore_empty_str();
    }

    size_t read_count = fread(buffer, 1, (size_t)length, file);
    fclose(file);
    return encore_from_owned_buffer(buffer, read_count);
}

int32_t encore_fs_write_file(encore_str path, encore_str contents) {
    char *path_c = encore_to_cstr(path);
    if (path_c == NULL) {
        return -1;
    }

    FILE *file = fopen(path_c, "wb");
    free(path_c);
    if (file == NULL) {
        return -1;
    }

    size_t contents_len = encore_str_size(contents);
    char *contents_data = encore_str_data(contents);
    size_t written = fwrite(contents_data, 1, contents_len, file);
    fclose(file);
    return written == contents_len ? 0 : -1;
}

typedef struct {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t block[64];
    size_t block_length;
} encore_sha256_context;

static uint32_t encore_sha256_rotate(uint32_t value, unsigned int bits) {
    return (value >> bits) | (value << (32U - bits));
}

static void encore_sha256_transform(encore_sha256_context *context, const uint8_t block[64]) {
    static const uint32_t constants[64] = {
        0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
        0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
        0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
        0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
        0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
        0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
        0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
        0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U
    };
    uint32_t words[64];
    for (size_t index = 0; index < 16; ++index) {
        size_t offset = index * 4;
        words[index] = ((uint32_t)block[offset] << 24) | ((uint32_t)block[offset + 1] << 16) |
                       ((uint32_t)block[offset + 2] << 8) | (uint32_t)block[offset + 3];
    }
    for (size_t index = 16; index < 64; ++index) {
        uint32_t x = words[index - 15];
        uint32_t y = words[index - 2];
        uint32_t small0 = encore_sha256_rotate(x, 7) ^ encore_sha256_rotate(x, 18) ^ (x >> 3);
        uint32_t small1 = encore_sha256_rotate(y, 17) ^ encore_sha256_rotate(y, 19) ^ (y >> 10);
        words[index] = words[index - 16] + small0 + words[index - 7] + small1;
    }
    uint32_t a = context->state[0], b = context->state[1], c = context->state[2], d = context->state[3];
    uint32_t e = context->state[4], f = context->state[5], g = context->state[6], h = context->state[7];
    for (size_t index = 0; index < 64; ++index) {
        uint32_t big1 = encore_sha256_rotate(e, 6) ^ encore_sha256_rotate(e, 11) ^ encore_sha256_rotate(e, 25);
        uint32_t choose = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + big1 + choose + constants[index] + words[index];
        uint32_t big0 = encore_sha256_rotate(a, 2) ^ encore_sha256_rotate(a, 13) ^ encore_sha256_rotate(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = big0 + majority;
        h = g; g = f; f = e; e = d + temp1; d = c; c = b; b = a; a = temp1 + temp2;
    }
    context->state[0] += a; context->state[1] += b; context->state[2] += c; context->state[3] += d;
    context->state[4] += e; context->state[5] += f; context->state[6] += g; context->state[7] += h;
}

static void encore_sha256_init(encore_sha256_context *context) {
    static const uint32_t initial[8] = {
        0x6a09e667U,0xbb67ae85U,0x3c6ef372U,0xa54ff53aU,
        0x510e527fU,0x9b05688cU,0x1f83d9abU,0x5be0cd19U
    };
    memcpy(context->state, initial, sizeof(initial));
    context->bit_count = 0;
    context->block_length = 0;
}

static void encore_sha256_update(encore_sha256_context *context, const uint8_t *data, size_t length) {
    context->bit_count += (uint64_t)length * 8U;
    while (length > 0) {
        size_t available = 64 - context->block_length;
        size_t count = length < available ? length : available;
        memcpy(context->block + context->block_length, data, count);
        context->block_length += count;
        data += count;
        length -= count;
        if (context->block_length == 64) {
            encore_sha256_transform(context, context->block);
            context->block_length = 0;
        }
    }
}

static void encore_sha256_finish(encore_sha256_context *context, uint8_t digest[32]) {
    context->block[context->block_length++] = 0x80U;
    if (context->block_length > 56) {
        memset(context->block + context->block_length, 0, 64 - context->block_length);
        encore_sha256_transform(context, context->block);
        context->block_length = 0;
    }
    memset(context->block + context->block_length, 0, 56 - context->block_length);
    for (size_t index = 0; index < 8; ++index) {
        context->block[63 - index] = (uint8_t)(context->bit_count >> (index * 8));
    }
    encore_sha256_transform(context, context->block);
    for (size_t index = 0; index < 8; ++index) {
        digest[index * 4] = (uint8_t)(context->state[index] >> 24);
        digest[index * 4 + 1] = (uint8_t)(context->state[index] >> 16);
        digest[index * 4 + 2] = (uint8_t)(context->state[index] >> 8);
        digest[index * 4 + 3] = (uint8_t)context->state[index];
    }
}

encore_str encore_fs_sha256(encore_str path) {
    char *path_c = encore_to_cstr(path);
    if (path_c == NULL) return encore_empty_str();
    FILE *file = fopen(path_c, "rb");
    free(path_c);
    if (file == NULL) return encore_empty_str();
    encore_sha256_context context;
    encore_sha256_init(&context);
    uint8_t buffer[65536];
    bool failed = false;
    for (;;) {
        size_t count = fread(buffer, 1, sizeof(buffer), file);
        if (count > 0) encore_sha256_update(&context, buffer, count);
        if (count < sizeof(buffer)) { if (ferror(file)) failed = true; break; }
    }
    if (fclose(file) != 0) failed = true;
    if (failed) return encore_empty_str();
    uint8_t digest[32];
    char encoded[65];
    static const char hexadecimal[] = "0123456789abcdef";
    encore_sha256_finish(&context, digest);
    for (size_t index = 0; index < 32; ++index) {
        encoded[index * 2] = hexadecimal[digest[index] >> 4];
        encoded[index * 2 + 1] = hexadecimal[digest[index] & 15U];
    }
    encoded[64] = '\0';
    return encore_from_cstr_copy(encoded);
}

int32_t encore_fs_status(encore_str path) {
    char *path_c = encore_to_cstr(path);
    if (path_c == NULL) {
        return -1;
    }

    struct stat st;
    int32_t result = stat(path_c, &st) == 0 ? 0 : -1;
    free(path_c);
    return result;
}

bool encore_fs_is_directory(encore_str path) {
    char *path_c = encore_to_cstr(path);
    if (path_c == NULL) return false;
    struct stat st;
    bool result = false;
    if (stat(path_c, &st) == 0) {
#ifdef _WIN32
        result = (st.st_mode & _S_IFDIR) != 0;
#else
        result = S_ISDIR(st.st_mode);
#endif
    }
    free(path_c);
    return result;
}

int32_t encore_fs_remove_file(encore_str path) {
    char *path_c = encore_to_cstr(path);
    if (path_c == NULL) {
        return -1;
    }

    int result = remove(path_c);
    free(path_c);
    return result == 0 ? 0 : -1;
}

int32_t encore_fs_copy_file(encore_str source, encore_str destination) {
    char *source_c = encore_to_cstr(source);
    char *destination_c = encore_to_cstr(destination);
    if (source_c == NULL || destination_c == NULL) { free(source_c); free(destination_c); return -1; }
    FILE *input = fopen(source_c, "rb");
    free(source_c);
    if (input == NULL) { free(destination_c); return -1; }

    size_t destination_len = strlen(destination_c);
    if (destination_len > SIZE_MAX - 32) { fclose(input); free(destination_c); return -1; }
    char *temporary_c = malloc(destination_len + 32);
    if (temporary_c == NULL) { fclose(input); free(destination_c); return -1; }
    memcpy(temporary_c, destination_c, destination_len);
#ifdef _WIN32
    int temporary_fd = -1;
    unsigned long process_id = GetCurrentProcessId();
    for (unsigned int attempt = 0; attempt < 100 && temporary_fd < 0; ++attempt) {
        snprintf(temporary_c + destination_len, 32, ".tmp.%lu.%u", process_id, attempt);
        temporary_fd = _open(temporary_c, _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY,
                             _S_IREAD | _S_IWRITE);
    }
#else
    memcpy(temporary_c + destination_len, ".tmp.XXXXXX", 12);
    int temporary_fd = mkstemp(temporary_c);
#endif
#ifdef _WIN32
    FILE *output = temporary_fd < 0 ? NULL : _fdopen(temporary_fd, "wb");
#else
    FILE *output = temporary_fd < 0 ? NULL : fdopen(temporary_fd, "wb");
#endif
    if (output == NULL) {
        if (temporary_fd >= 0) {
#ifdef _WIN32
            _close(temporary_fd);
#else
            close(temporary_fd);
#endif
        }
        fclose(input);
        remove(temporary_c);
        free(temporary_c);
        free(destination_c);
        return -1;
    }
    char buffer[65536];
    int32_t status = 0;
    for (;;) {
        size_t count = fread(buffer, 1, sizeof(buffer), input);
        if (count > 0 && fwrite(buffer, 1, count, output) != count) { status = -1; break; }
        if (count < sizeof(buffer)) { if (ferror(input)) status = -1; break; }
    }
    if (fclose(input) != 0 || fclose(output) != 0) status = -1;
    if (status == 0) {
#ifdef _WIN32
        if (!MoveFileExA(temporary_c, destination_c,
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) status = -1;
#else
        if (rename(temporary_c, destination_c) != 0) status = -1;
#endif
    }
    if (status != 0) remove(temporary_c);
    free(temporary_c);
    free(destination_c);
    return status;
}

int32_t encore_fs_set_executable(encore_str path) {
#ifdef _WIN32
    (void)path;
    return 0;
#else
    char *path_c = encore_to_cstr(path);
    if (path_c == NULL) return -1;
    struct stat info;
    int32_t status = stat(path_c, &info) == 0 && chmod(path_c, info.st_mode | S_IXUSR | S_IXGRP | S_IXOTH) == 0 ? 0 : -1;
    free(path_c);
    return status;
#endif
}

int32_t encore_fs_mkdir(encore_str path) {
    char *path_c = encore_to_cstr(path);
    if (path_c == NULL) {
        return -1;
    }

    int rc =
#ifdef _WIN32
        _mkdir(path_c);
#else
        mkdirat(AT_FDCWD, path_c, 0755);
#endif
    int32_t status = (rc == 0 || errno == EEXIST) ? 0 : -1;
    free(path_c);
    return status;
}

int32_t encore_fs_mkdir_all(encore_str path) {
    char *path_c = encore_to_cstr(path);
    if (path_c == NULL || path_c[0] == '\0') { free(path_c); return -1; }
    size_t len = strlen(path_c);
    for (size_t index = 1; index <= len; index += 1) {
        bool boundary = index == len || path_c[index] == '/' || path_c[index] == '\\';
        if (!boundary) continue;
#ifdef _WIN32
        if (index == 2 && path_c[1] == ':') continue;
#endif
        char saved = path_c[index];
        path_c[index] = '\0';
        if (path_c[0] != '\0') {
            int rc =
#ifdef _WIN32
                _mkdir(path_c);
#else
                mkdirat(AT_FDCWD, path_c, 0755);
#endif
            if (rc != 0 && errno != EEXIST) { free(path_c); return -1; }
        }
        path_c[index] = saved;
    }
    free(path_c);
    return 0;
}

static bool encore_append_dir_entry(char **buffer, size_t *cap, size_t *len, const char *name) {
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return true;
    }

    size_t name_len = strlen(name);
    size_t need = *len + name_len + 1;
    if (need > *cap) {
        size_t next_cap = *cap;
        while (need > next_cap) {
            if (next_cap > (SIZE_MAX / 2)) {
                return false;
            }
            next_cap *= 2;
        }
        char *next = realloc(*buffer, next_cap + 1);
        if (next == NULL) {
            return false;
        }
        *buffer = next;
        *cap = next_cap;
    }

    memcpy(*buffer + *len, name, name_len);
    *len += name_len;
    (*buffer)[(*len)++] = '\n';
    return true;
}

encore_str encore_fs_read_dir(encore_str path) {
    char *path_c = encore_to_cstr(path);
    if (path_c == NULL) {
        return encore_empty_str();
    }

#ifdef _WIN32
    size_t path_len = strlen(path_c);
    const char *suffix = (path_len > 0 && (path_c[path_len - 1] == '\\' || path_c[path_len - 1] == '/')) ? "*" : "\\*";
    char *pattern = malloc(path_len + strlen(suffix) + 1);
    if (pattern == NULL) {
        free(path_c);
        return encore_empty_str();
    }
    strcpy(pattern, path_c);
    strcat(pattern, suffix);
    free(path_c);

    WIN32_FIND_DATAA data;
    HANDLE handle = FindFirstFileA(pattern, &data);
    free(pattern);
    if (handle == INVALID_HANDLE_VALUE) {
        return encore_empty_str();
    }
#else
    DIR *dir = opendir(path_c);
    free(path_c);
    if (dir == NULL) {
        return encore_empty_str();
    }
#endif

    size_t cap = 256;
    size_t len = 0;
    char *buffer = malloc(cap + 1);
    if (buffer == NULL) {
#ifdef _WIN32
        FindClose(handle);
#else
        closedir(dir);
#endif
        return encore_empty_str();
    }

#ifdef _WIN32
    do {
        if (!encore_append_dir_entry(&buffer, &cap, &len, data.cFileName)) {
            free(buffer);
            FindClose(handle);
            return encore_empty_str();
        }
    } while (FindNextFileA(handle, &data));
    FindClose(handle);
#else
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (!encore_append_dir_entry(&buffer, &cap, &len, entry->d_name)) {
            free(buffer);
            closedir(dir);
            return encore_empty_str();
        }
    }
    closedir(dir);
#endif

    if (len > 0 && buffer[len - 1] == '\n') {
        len -= 1;
    }
    return encore_from_owned_buffer(buffer, len);
}

static int encore_remove_tree_c(const char *path) {
#ifdef _WIN32
    DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? (RemoveDirectoryA(path) ? 0 : -1)
                                                           : (DeleteFileA(path) ? 0 : -1);
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
        return DeleteFileA(path) ? 0 : -1;
    }
    size_t length = strlen(path);
    char *pattern = malloc(length + 3);
    if (pattern == NULL) return -1;
    snprintf(pattern, length + 3, "%s\\*", path);
    WIN32_FIND_DATAA data;
    HANDLE search = FindFirstFileA(pattern, &data);
    free(pattern);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) continue;
            size_t child_length = length + strlen(data.cFileName) + 2;
            char *child = malloc(child_length);
            if (child == NULL) { FindClose(search); return -1; }
            snprintf(child, child_length, "%s\\%s", path, data.cFileName);
            int status = encore_remove_tree_c(child);
            free(child);
            if (status != 0) { FindClose(search); return -1; }
        } while (FindNextFileA(search, &data));
        FindClose(search);
    }
    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
    return RemoveDirectoryA(path) ? 0 : -1;
#else
    struct stat info;
    if (lstat(path, &info) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(info.st_mode) || S_ISLNK(info.st_mode)) return unlink(path) == 0 ? 0 : -1;
    DIR *directory = opendir(path);
    if (directory == NULL) return -1;
    struct dirent *entry;
    int status = 0;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        size_t child_length = strlen(path) + strlen(entry->d_name) + 2;
        char *child = malloc(child_length);
        if (child == NULL) { status = -1; break; }
        snprintf(child, child_length, "%s/%s", path, entry->d_name);
        if (encore_remove_tree_c(child) != 0) status = -1;
        free(child);
        if (status != 0) break;
    }
    closedir(directory);
    if (status != 0) return -1;
    return rmdir(path) == 0 ? 0 : -1;
#endif
}

int32_t encore_fs_remove_dir_all(encore_str path) {
    char *path_c = encore_to_cstr(path);
    if (path_c == NULL) return -1;
    int32_t status = encore_remove_tree_c(path_c);
    free(path_c);
    return status;
}

static int encore_move_path_c(const char *source, const char *destination) {
#ifdef _WIN32
    return MoveFileExA(source, destination, MOVEFILE_WRITE_THROUGH) ? 0 : -1;
#else
    return rename(source, destination) == 0 ? 0 : -1;
#endif
}

static int encore_replace_installation_c(const char *staged, const char *root) {
    size_t backup_length = strlen(root) + 19;
    char *backup = malloc(backup_length);
    if (backup == NULL) return -1;
    snprintf(backup, backup_length, "%s.previous", root);
    if (encore_remove_tree_c(backup) != 0) { free(backup); return -1; }
    if (encore_move_path_c(root, backup) != 0) { free(backup); return -1; }
    if (encore_move_path_c(staged, root) != 0) {
        encore_move_path_c(backup, root);
        free(backup);
        return -1;
    }
    (void)encore_remove_tree_c(backup);
    free(backup);
    return 0;
}

#ifdef _WIN32
static bool encore_windows_safe_argument(const char *value) {
    return value != NULL && strchr(value, '"') == NULL && strchr(value, '\r') == NULL && strchr(value, '\n') == NULL;
}

static char *encore_windows_update_command(const char *helper, DWORD parent, const char *staged,
                                           const char *root, const char *workspace) {
    if (!encore_windows_safe_argument(helper) || !encore_windows_safe_argument(staged) ||
        !encore_windows_safe_argument(root) || !encore_windows_safe_argument(workspace)) return NULL;
    size_t capacity = strlen(helper) * 2 + strlen(staged) * 2 + strlen(root) * 2 + strlen(workspace) * 2 + 160;
    char *command = malloc(capacity);
    if (command == NULL) return NULL;
    snprintf(command, capacity, "\"%s\" __self_update_apply %lu \"%s\" \"%s\" \"%s\" \"%s\"",
             helper, (unsigned long)parent, staged, root, workspace, helper);
    return command;
}
#endif

int32_t encore_self_update_commit(encore_str staged_root, encore_str install_root,
                                  encore_str new_executable, encore_str workspace) {
    char *staged = encore_to_cstr(staged_root);
    char *root = encore_to_cstr(install_root);
    char *new_binary = encore_to_cstr(new_executable);
    char *work = encore_to_cstr(workspace);
    if (staged == NULL || root == NULL || new_binary == NULL || work == NULL) {
        free(staged); free(root); free(new_binary); free(work); return -1;
    }
#ifdef _WIN32
    size_t helper_length = strlen(root) + 40;
    char *helper = malloc(helper_length);
    if (helper == NULL) { free(staged); free(root); free(new_binary); free(work); return -1; }
    snprintf(helper, helper_length, "%s.update-helper-%lu.exe", root, (unsigned long)GetCurrentProcessId());
    encore_str source_value = encore_from_cstr_copy(new_binary);
    encore_str helper_value = encore_from_cstr_copy(helper);
    if (encore_fs_copy_file(source_value, helper_value) != 0) {
        free(helper); free(staged); free(root); free(new_binary); free(work); return -1;
    }
    char *command = encore_windows_update_command(helper, GetCurrentProcessId(), staged, root, work);
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    memset(&startup, 0, sizeof(startup)); startup.cb = sizeof(startup);
    memset(&process, 0, sizeof(process));
    BOOL started = command != NULL && CreateProcessA(helper, command, NULL, NULL, FALSE,
                                                      CREATE_NO_WINDOW | DETACHED_PROCESS,
                                                      NULL, NULL, &startup, &process);
    if (started) { CloseHandle(process.hThread); CloseHandle(process.hProcess); }
    free(command); free(helper); free(staged); free(root); free(new_binary); free(work);
    return started ? 1 : -1;
#else
    int32_t status = encore_replace_installation_c(staged, root);
    if (status == 0) encore_remove_tree_c(work);
    free(staged); free(root); free(new_binary); free(work);
    return status;
#endif
}

int32_t encore_self_update_apply(size_t parent_pid, encore_str staged_root, encore_str install_root,
                                 encore_str workspace, encore_str helper_path) {
#ifdef _WIN32
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)parent_pid);
    if (parent != NULL) { WaitForSingleObject(parent, INFINITE); CloseHandle(parent); }
    char *staged = encore_to_cstr(staged_root);
    char *root = encore_to_cstr(install_root);
    char *work = encore_to_cstr(workspace);
    char *helper = encore_to_cstr(helper_path);
    if (staged == NULL || root == NULL || work == NULL || helper == NULL) {
        free(staged); free(root); free(work); free(helper); return -1;
    }
    int32_t status = encore_replace_installation_c(staged, root);
    if (status == 0) encore_remove_tree_c(work);
    MoveFileExA(helper, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    free(staged); free(root); free(work); free(helper);
    return status;
#else
    (void)parent_pid; (void)staged_root; (void)install_root; (void)workspace; (void)helper_path;
    return -1;
#endif
}
