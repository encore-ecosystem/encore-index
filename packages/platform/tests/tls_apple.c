/* Exercise the real adapter without bootstrapping the language compiler. */
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
typedef struct { char *data; size_t size; } encore_str;
static encore_str encore_empty_str(void) { return (encore_str){NULL, 0}; }
static encore_str encore_from_owned_buffer(char *p, size_t n) { return (encore_str){p, n}; }
static char *encore_str_data(encore_str s) { return s.data; }
static size_t encore_str_size(encore_str s) { return s.size; }
static char *encore_to_cstr(encore_str s) {
    char *p = malloc(s.size + 1);
    if (p) { memcpy(p, s.data, s.size); p[s.size] = 0; }
    return p;
}
static void encore_set_net_error_cstr(const char *message) { (void)message; }
#include "../tls_apple.h"
static encore_str text(char *p) { return (encore_str){p, strlen(p)}; }
static int probe(int argc, char **argv) {
    if (argc != 6) return 99;
    bool read_timeout = !strcmp(argv[5], "timeout");
    size_t timeout = strtoul(argv[4], NULL, 10);
    size_t handle = encore_tls_client_connect(text(argv[1]), strtoul(argv[2], NULL, 10),
        text(argv[3]), read_timeout ? 2000 : timeout);
    if (!strcmp(argv[5], "reject")) {
        if (handle) encore_tls_close(handle);
        return handle ? 1 : 0;
    }
    if (!handle) return 2;
    if (encore_tls_write(handle, text("hello")) != 5) { encore_tls_close(handle); return 3; }
    if (read_timeout) {
        /* Test the read deadline independently of trust-service startup. */
        ((encore_tls_client *)(uintptr_t)handle)->timeout_ms = timeout;
        encore_str data = encore_tls_read(handle, 1024);
        bool failed = encore_tls_read_failed(handle);
        free(data.data);
        encore_tls_close(handle);
        return failed ? 0 : 4;
    }
    char received[5]; size_t count = 0;
    while (count < sizeof(received)) {
        encore_str data = encore_tls_read(handle, 1);
        bool bad = encore_tls_read_failed(handle) || data.size != 1;
        if (!bad) received[count++] = data.data[0];
        free(data.data);
        if (bad) { encore_tls_close(handle); return 5; }
    }
    encore_str eof = encore_tls_read(handle, 10);
    bool bad = eof.size != 0 || encore_tls_read_failed(handle) || memcmp(received, "hello", 5);
    free(eof.data);
    encore_tls_close(handle);
    return bad ? 6 : 0;
}
int main(int argc, char **argv) {
    int result = probe(argc, argv);
    /* Let cancellation completions execute under ASan before process exit. */
    sleep(1);
    return result;
}
