/* Test-only libsecret stand-in for separate CLI processes. Never linked into
 * production; tests load this .so from a disposable LD_LIBRARY_PATH directory.
 * The private fixture file is deliberately NOT a production storage backend. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

void *secret_schema_new(const char *name, int flags, ...) {
    (void)name; (void)flags;
    return (void *)(uintptr_t)1;
}
static const char *path(void) { return getenv("ENCORE_TEST_KEYRING_FILE"); }
char *secret_password_lookup_sync(const void *schema, void *cancel, void **error, ...) {
    (void)schema; (void)cancel; (void)error;
    va_list args;
    va_start(args, error);
    (void)va_arg(args, const char *);
    const char *key = va_arg(args, const char *);
    va_end(args);
    FILE *file = fopen(path(), "r");
    if (!file) return NULL;
    char found[1025], value[2049];
    int read = fscanf(file, "%1024s\n%2048s", found, value);
    fclose(file);
    return read == 2 && !strcmp(found, key) ? strdup(value) : NULL;
}
int secret_password_store_sync(const void *schema, const char *collection, const char *label,
                               const char *value, void *cancel, void **error, ...) {
    (void)schema; (void)collection; (void)label; (void)cancel; (void)error;
    va_list args;
    va_start(args, error);
    (void)va_arg(args, const char *);
    const char *key = va_arg(args, const char *);
    va_end(args);
    FILE *file = fopen(path(), "w");
    if (!file) return 0;
    int ok = fprintf(file, "%s\n%s\n", key, value) > 0;
    return fclose(file) == 0 && ok;
}
int secret_password_clear_sync(const void *schema, void *cancel, void **error, ...) {
    (void)schema; (void)cancel; (void)error;
    return remove(path()) == 0;
}
void secret_password_free(void *value) { free(value); }
void g_error_free(void *value) { free(value); }
int g_error_matches(const void *error, uint32_t domain, int code) {
    (void)error; (void)domain; (void)code;
    return 0;
}
uint32_t secret_error_get_quark(void) { return 1; }
uint32_t g_io_error_quark(void) { return 2; }
