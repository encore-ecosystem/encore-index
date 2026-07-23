#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#endif

typedef struct {
    size_t ref_count;
    size_t len;
    char data[];
} encore_str_object;

typedef struct {
    encore_str_object *object;
} encore_str;

extern void *encore_str_from_cstr(const char *value);
extern encore_str encore_str_from_codepoint(size_t value);

static encore_str rich_str(const char *value) {
    encore_str result = {(encore_str_object *)encore_str_from_cstr(value)};
    return result;
}

typedef struct {
    const char *const *frames;
    size_t count;
} rich_spinner_frames;

#define RICH_SPINNER_FRAMES(name, ...) \
    static const char *const name##_items[] = {__VA_ARGS__}; \
    static const rich_spinner_frames name = {name##_items, sizeof(name##_items) / sizeof(name##_items[0])}

RICH_SPINNER_FRAMES(g_dots, "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏");
RICH_SPINNER_FRAMES(g_dots2, "⣾", "⣽", "⣻", "⢿", "⡿", "⣟", "⣯", "⣷");
RICH_SPINNER_FRAMES(g_dots3, "⠋", "⠙", "⠚", "⠞", "⠖", "⠦", "⠴", "⠲", "⠳", "⠓");
RICH_SPINNER_FRAMES(g_dots4, "⠄", "⠆", "⠇", "⠋", "⠙", "⠸", "⠰", "⠠", "⠰", "⠸", "⠙", "⠋", "⠇", "⠆");
RICH_SPINNER_FRAMES(g_dots5, "⠋", "⠙", "⠚", "⠒", "⠂", "⠂", "⠒", "⠲", "⠴", "⠦", "⠖", "⠒", "⠐", "⠐", "⠒", "⠓", "⠋");
RICH_SPINNER_FRAMES(g_dots6, "⠁", "⠉", "⠙", "⠚", "⠒", "⠂", "⠂", "⠒", "⠲", "⠴", "⠤", "⠄", "⠄", "⠤", "⠴", "⠲", "⠒", "⠂", "⠂", "⠒", "⠚", "⠙", "⠉", "⠁");
RICH_SPINNER_FRAMES(g_dots7, "⠈", "⠉", "⠋", "⠓", "⠒", "⠐", "⠐", "⠒", "⠖", "⠦", "⠤", "⠠", "⠠", "⠤", "⠦", "⠖", "⠒", "⠐", "⠐", "⠒", "⠓", "⠋", "⠉", "⠈");
RICH_SPINNER_FRAMES(g_dots8, "⠁", "⠁", "⠉", "⠙", "⠚", "⠒", "⠂", "⠂", "⠒", "⠲", "⠴", "⠤", "⠄", "⠄", "⠤", "⠠", "⠠", "⠤", "⠦", "⠖", "⠒", "⠐", "⠐", "⠒", "⠓", "⠋", "⠉", "⠈", "⠈");
RICH_SPINNER_FRAMES(g_dots9, "⢹", "⢺", "⢼", "⣸", "⣇", "⡧", "⡗", "⡏");
RICH_SPINNER_FRAMES(g_dots10, "⢄", "⢂", "⢁", "⡁", "⡈", "⡐", "⡠");
RICH_SPINNER_FRAMES(g_dots11, "⠁", "⠂", "⠄", "⡀", "⢀", "⠠", "⠐", "⠈");
RICH_SPINNER_FRAMES(g_dots12,
    "⢀⠀", "⡀⠀", "⠄⠀", "⢂⠀", "⡂⠀", "⠅⠀", "⢃⠀", "⡃⠀", "⠍⠀", "⢋⠀", "⡋⠀",
    "⠍⠁", "⢋⠁", "⡋⠁", "⠍⠉", "⠋⠉", "⠋⠉", "⠉⠙", "⠉⠙", "⠉⠩", "⠈⢙", "⠈⡙",
    "⢈⠩", "⡀⢙", "⠄⡙", "⢂⠩", "⡂⢘", "⠅⡘", "⢃⠨", "⡃⢐", "⠍⡐", "⢋⠠", "⡋⢀",
    "⠍⡁", "⢋⠁", "⡋⠁", "⠍⠉", "⠋⠉", "⠋⠉", "⠉⠙", "⠉⠙", "⠉⠩", "⠈⢙", "⠈⡙",
    "⠈⠩", "⠀⢙", "⠀⡙", "⠀⠩", "⠀⢘", "⠀⡘", "⠀⠨", "⠀⢐", "⠀⡐", "⠀⠠", "⠀⢀", "⠀⡀");
RICH_SPINNER_FRAMES(g_dots13, "⣼", "⣹", "⢻", "⠿", "⡟", "⣏", "⣧", "⣶");
RICH_SPINNER_FRAMES(g_dots14, "⠉⠉", "⠈⠙", "⠀⠹", "⠀⢸", "⠀⢰", "⢀⣰", "⣀⣀", "⡄⢀", "⡆⠀", "⡇⠀", "⠏⠀", "⠋⠁");
RICH_SPINNER_FRAMES(g_dots_circle, "⢎ ", "⠎⠁", "⠊⠑", "⠈⠱", " ⡱", "⢀⡰", "⢄⡠", "⢆⡀");
RICH_SPINNER_FRAMES(g_line, "-", "\\", "|", "/");
RICH_SPINNER_FRAMES(g_arc, ".", "o", "O", "o");

static const rich_spinner_frames *known_frames(size_t style) {
    static const rich_spinner_frames *const styles[] = {
        &g_dots, &g_dots2, &g_dots3, &g_dots4, &g_dots5, &g_dots6,
        &g_dots7, &g_dots8, NULL, &g_dots9, &g_dots10, &g_dots11,
        &g_dots12, &g_dots13, &g_dots14, &g_dots_circle, &g_line, &g_arc
    };
    return style < sizeof(styles) / sizeof(styles[0]) ? styles[style] : &g_dots;
}

size_t encore_rich_spinner_frame_count(size_t style) {
    if (style == 8) return 256;
    const rich_spinner_frames *known = known_frames(style);
    return known == NULL ? 0 : known->count;
}

encore_str encore_rich_spinner_frame(size_t style, size_t index) {
    if (style == 8) {
        size_t value = index % 256;
        size_t offset = value % 8;
        if ((value / 8) % 2 != 0) offset += 64;
        if ((value / 16) % 2 != 0) offset += 8;
        if ((value / 32) % 2 != 0) offset += 16;
        if ((value / 64) % 2 != 0) offset += 32;
        if ((value / 128) % 2 != 0) offset += 128;
        return encore_str_from_codepoint(0x2800 + offset);
    }
    const rich_spinner_frames *known = known_frames(style);
    if (known == NULL || known->count == 0) return rich_str("");
    return rich_str(known->frames[index % known->count]);
}

bool encore_rich_is_terminal(int32_t fd) {
#ifdef _WIN32
    return fd >= 0 && _isatty(fd) != 0;
#else
    return fd >= 0 && isatty(fd) != 0;
#endif
}

static _Atomic bool g_running = false;
static bool g_started = false;
static char *g_message;
static char *g_frames;
static size_t g_frame_count;
static uint64_t g_interval_ms = 80;

#ifdef _WIN32
static HANDLE g_thread;
static SRWLOCK g_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_wake = CONDITION_VARIABLE_INIT;
#else
static pthread_t g_thread;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wake = PTHREAD_COND_INITIALIZER;
#endif

static void lock_spinner(void) {
#ifdef _WIN32
    AcquireSRWLockExclusive(&g_lock);
#else
    pthread_mutex_lock(&g_lock);
#endif
}

static void unlock_spinner(void) {
#ifdef _WIN32
    ReleaseSRWLockExclusive(&g_lock);
#else
    pthread_mutex_unlock(&g_lock);
#endif
}

static void wait_for_frame(void) {
    lock_spinner();
    if (atomic_load(&g_running)) {
#ifdef _WIN32
        SleepConditionVariableSRW(&g_wake, &g_lock,
                                  g_interval_ms > UINT32_MAX ? UINT32_MAX : (DWORD)g_interval_ms, 0);
#else
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += (time_t)(g_interval_ms / 1000);
        deadline.tv_nsec += (long)((g_interval_ms % 1000) * 1000000);
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&g_wake, &g_lock, &deadline);
#endif
    }
    unlock_spinner();
}

static void wake_thread(void) {
    lock_spinner();
#ifdef _WIN32
    WakeAllConditionVariable(&g_wake);
#else
    pthread_cond_broadcast(&g_wake);
#endif
    unlock_spinner();
}

static char *copy_string(encore_str value) {
    size_t len = value.object == NULL ? 0 : value.object->len;
    char *copy = (char *)malloc(len + 1);
    if (copy == NULL) return NULL;
    if (len > 0) memcpy(copy, value.object->data, len);
    copy[len] = '\0';
    return copy;
}

static void set_message(encore_str message) {
    char *copy = copy_string(message);
    if (copy == NULL) return;
    lock_spinner();
    char *previous = g_message;
    g_message = copy;
    unlock_spinner();
    free(previous);
}

static size_t count_frames(const char *frames) {
    if (frames == NULL || frames[0] == '\0') return 0;
    size_t count = 1;
    for (const char *cursor = frames; *cursor != '\0'; ++cursor) {
        if (*cursor == '\n') count += 1;
    }
    return count;
}

static void write_frame(size_t index) {
    const char *start = g_frames;
    if (start == NULL || g_frame_count == 0) {
        fputc('.', stderr);
        return;
    }
    index %= g_frame_count;
    while (index-- > 0) {
        const char *separator = strchr(start, '\n');
        if (separator == NULL) break;
        start = separator + 1;
    }
    const char *end = strchr(start, '\n');
    fwrite(start, 1, end == NULL ? strlen(start) : (size_t)(end - start), stderr);
}

#ifdef _WIN32
static DWORD WINAPI spinner_main(LPVOID unused) {
    (void)unused;
#else
static void *spinner_main(void *unused) {
    (void)unused;
#endif
    size_t tick = 0;
    while (atomic_load(&g_running)) {
        lock_spinner();
        fputs("\r\x1b[2K\x1b[0;36m", stderr);
        write_frame(tick++);
        fprintf(stderr, "\x1b[0m %s", g_message == NULL ? "" : g_message);
        fflush(stderr);
        unlock_spinner();
        wait_for_frame();
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

void encore_rich_terminal_spinner_stop(void);

bool encore_rich_terminal_spinner_start(encore_str message, encore_str frames, size_t interval_ms) {
    if (!encore_rich_is_terminal(2)) return false;
    encore_rich_terminal_spinner_stop();
    set_message(message);
    g_frames = copy_string(frames);
    g_frame_count = count_frames(g_frames);
    g_interval_ms = interval_ms == 0 ? 80 : (uint64_t)interval_ms;
    atomic_store(&g_running, true);
#ifdef _WIN32
    g_thread = CreateThread(NULL, 0, spinner_main, NULL, 0, NULL);
    g_started = g_thread != NULL;
#else
    g_started = pthread_create(&g_thread, NULL, spinner_main, NULL) == 0;
#endif
    if (!g_started) {
        atomic_store(&g_running, false);
        free(g_message);
        free(g_frames);
        g_message = NULL;
        g_frames = NULL;
        g_frame_count = 0;
    }
    return g_started;
}

bool encore_rich_terminal_spinner_update(encore_str message) {
    if (!g_started) return false;
    set_message(message);
    return true;
}

void encore_rich_terminal_spinner_stop(void) {
    if (!g_started) return;
    atomic_store(&g_running, false);
    wake_thread();
#ifdef _WIN32
    WaitForSingleObject(g_thread, INFINITE);
    CloseHandle(g_thread);
    g_thread = NULL;
#else
    pthread_join(g_thread, NULL);
#endif
    g_started = false;
    free(g_message);
    free(g_frames);
    g_message = NULL;
    g_frames = NULL;
    g_frame_count = 0;
    fputs("\r\x1b[2K", stderr);
    fflush(stderr);
}

#undef RICH_SPINNER_FRAMES
