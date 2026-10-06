/* Checked, buffered file IO. Handles are registry IDs, never raw pointers.
 * Closing invalidates every Encore copy; IDs are never reused. */
typedef struct encore_checked_file {
    uint64_t id;
    FILE *stream;
    size_t users;
    struct encore_checked_file *next;
} encore_checked_file;
static encore_checked_file *encore_files;
static uint64_t encore_next_file = 1;
static atomic_flag encore_files_lock = ATOMIC_FLAG_INIT;
#ifdef _MSC_VER
static __declspec(thread) int encore_file_error;
static __declspec(thread) bool encore_file_eof;
#else
static _Thread_local int encore_file_error;
static _Thread_local bool encore_file_eof;
#endif
static void encore_file_lock(void) {
    while (atomic_flag_test_and_set_explicit(&encore_files_lock, memory_order_acquire)) {}
}
static void encore_file_unlock(void) {
    atomic_flag_clear_explicit(&encore_files_lock, memory_order_release);
}
static encore_checked_file *encore_file_acquire(uint64_t id) {
    encore_file_error = 0;
    encore_file_eof = false;
    encore_file_lock();
    encore_checked_file *entry = encore_files;
    while (entry && entry->id != id) entry = entry->next;
    if (entry) entry->users++;
    encore_file_unlock();
    if (!entry) encore_file_error = EBADF;
    return entry;
}
static void encore_file_release(encore_checked_file *entry) {
    encore_file_lock();
    entry->users--;
    encore_file_unlock();
}
int32_t encore_file_error_code(void) { return encore_file_error; }
int32_t encore_file_error_kind(void) {
    switch (encore_file_error) {
    case ENOENT: return 1;
    case EACCES: case EPERM: return 2;
    case EBADF: return 3;
    case EINVAL: return 4;
    case EBUSY: return 5;
    default: return 0;
    }
}
encore_str encore_file_error_message(void) {
    /* Numeric code remains available even on platforms with localized messages. */
    return encore_from_cstr_copy(strerror(encore_file_error));
}
bool encore_file_at_eof(void) { return encore_file_eof; }
uint64_t encore_file_open(encore_str path, bool writing) {
    encore_file_error = 0;
    size_t length = encore_str_size(path);
    if (memchr(encore_str_data(path), 0, length)) { encore_file_error = EINVAL; return 0; }
    char *name = encore_to_cstr(path);
    if (!name) { encore_file_error = ENOMEM; return 0; }
    FILE *stream = fopen(name, writing ? "wb" : "rb");
    int error = errno;
    free(name);
    if (!stream) { encore_file_error = error; return 0; }
    encore_checked_file *entry = malloc(sizeof(*entry));
    if (!entry) { fclose(stream); encore_file_error = ENOMEM; return 0; }
    encore_file_lock();
    if (encore_next_file == UINT64_MAX) {
        encore_file_unlock(); fclose(stream); free(entry);
        encore_file_error = EOVERFLOW; return 0;
    }
    entry->id = encore_next_file++;
    entry->stream = stream;
    entry->users = 0;
    entry->next = encore_files;
    encore_files = entry;
    encore_file_unlock();
    return entry->id;
}
bool encore_file_close(uint64_t id) {
    encore_file_error = 0;
    encore_file_lock();
    encore_checked_file **slot = &encore_files;
    while (*slot && (*slot)->id != id) slot = &(*slot)->next;
    if (!*slot) { encore_file_unlock(); return true; }
    encore_checked_file *entry = *slot;
    if (entry->users) {
        encore_file_unlock(); encore_file_error = EBUSY; return false;
    }
    *slot = entry->next;
    encore_file_unlock();
    int result = fclose(entry->stream);
    if (result) encore_file_error = errno ? errno : EIO;
    free(entry);
    return result == 0;
}
encore_str encore_file_read(uint64_t id, size_t size) {
    encore_checked_file *entry = encore_file_acquire(id);
    if (!entry) return encore_empty_str();
    if (size == SIZE_MAX) {
        encore_file_release(entry); encore_file_error = EINVAL; return encore_empty_str();
    }
    char *buffer = malloc(size + 1);
    if (!buffer) {
        encore_file_release(entry); encore_file_error = ENOMEM; return encore_empty_str();
    }
    size_t count = fread(buffer, 1, size, entry->stream);
    if (ferror(entry->stream)) encore_file_error = errno ? errno : EIO;
    encore_file_eof = feof(entry->stream) != 0;
    encore_file_release(entry);
    return encore_from_owned_buffer(buffer, count);
}
/* Geometric growth also supports pipes/non-seekable files, without trusting
 * a potentially stale stat size. Reserve one byte for the string terminator. */
encore_str encore_file_read_all(uint64_t id) {
    encore_checked_file *entry = encore_file_acquire(id);
    if (!entry) return encore_empty_str();
    size_t len = 0, capacity = 65536;
    char *buffer = malloc(capacity);
    if (!buffer) {
        encore_file_release(entry); encore_file_error = ENOMEM;
        return encore_empty_str();
    }
    for (;;) {
        if (len == capacity - 1) {
            if (capacity > SIZE_MAX / 2) { encore_file_error = EOVERFLOW; break; }
            char *next = realloc(buffer, capacity * 2);
            if (!next) { encore_file_error = ENOMEM; break; }
            buffer = next;
            capacity *= 2;
        }
        errno = 0;
        len += fread(buffer + len, 1, capacity - 1 - len, entry->stream);
        if (ferror(entry->stream)) {
            if (errno == EINTR) { clearerr(entry->stream); continue; }
            encore_file_error = errno ? errno : EIO;
            break;
        }
        if (feof(entry->stream)) { encore_file_eof = true; break; }
    }
    encore_file_release(entry);
    return encore_from_owned_buffer(buffer, len);
}
static encore_str encore_checked_line(FILE *stream) {
    encore_file_error = 0;
    encore_file_eof = false;
    size_t len = 0, capacity = 256;
    char *buffer = malloc(capacity);
    if (!buffer) { encore_file_error = ENOMEM; return encore_empty_str(); }
    for (;;) {
        int ch = fgetc(stream);
        if (ch == EOF) {
            if (ferror(stream)) {
                if (errno == EINTR) { clearerr(stream); continue; }
                encore_file_error = errno ? errno : EIO;
            }
            encore_file_eof = len == 0 && !encore_file_error;
            break;
        }
        if (ch == '\n') {
            if (len && buffer[len - 1] == '\r') len--;
            break;
        }
        if (len + 1 == capacity) {
            if (capacity > SIZE_MAX / 2) { encore_file_error = EOVERFLOW; break; }
            char *next = realloc(buffer, capacity * 2);
            if (!next) { encore_file_error = ENOMEM; break; }
            buffer = next; capacity *= 2;
        }
        buffer[len++] = (char)ch;
    }
    return encore_from_owned_buffer(buffer, len);
}
encore_str encore_file_read_line(uint64_t id) {
    encore_checked_file *entry = encore_file_acquire(id);
    if (!entry) return encore_empty_str();
    encore_str out = encore_checked_line(entry->stream);
    encore_file_release(entry);
    return out;
}
bool encore_file_write(uint64_t id, encore_str data) {
    encore_checked_file *entry = encore_file_acquire(id);
    if (!entry) return false;
    size_t size = encore_str_size(data);
    size_t count = fwrite(encore_str_data(data), 1, size, entry->stream);
    if (count != size) encore_file_error = errno ? errno : EIO;
    encore_file_release(entry);
    return count == size;
}
bool encore_file_flush(uint64_t id) {
    encore_checked_file *entry = encore_file_acquire(id);
    if (!entry) return false;
    int result = fflush(entry->stream);
    if (result) encore_file_error = errno ? errno : EIO;
    encore_file_release(entry);
    return result == 0;
}
encore_str encore_checked_input(encore_str prompt) {
    encore_file_error = 0;
    encore_file_eof = false;
    size_t size = encore_str_size(prompt);
    if (fwrite(encore_str_data(prompt), 1, size, stdout) != size || fflush(stdout) != 0) {
        encore_file_error = errno ? errno : EIO;
        return encore_empty_str();
    }
    return encore_checked_line(stdin);
}
