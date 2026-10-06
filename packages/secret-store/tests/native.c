/* Tests the native ABI without linking the compiler runtime. Real keyring
 * writes are opt-in and confined to a fresh random test key. */
#define _POSIX_C_SOURCE 200809L
#include "../runtime.c"
#include <assert.h>
#include <stdio.h>
struct encore_str_object {
  size_t size;
  char data[];
};
char *encore_str_data(encore_str s) { return s.object->data; }
size_t encore_str_size(encore_str s) { return s.object->size; }
encore_str encore_str_from_buffer(const void *p, size_t n) {
  encore_str s = {malloc(sizeof(*s.object) + n + 1)};
  assert(s.object);
  s.object->size = n;
  memcpy(s.object->data, p, n);
  s.object->data[n] = 0;
  return s;
}
static encore_str text(const char *s) {
  return encore_str_from_buffer(s, strlen(s));
}
static void check(int operation, const char *key, const char *value,
                  const char *expected) {
  encore_str k = text(key), v = text(value);
  encore_str r = encore_secret_store(operation, k, v);
  assert(!strcmp(encore_str_data(r), expected));
  free(k.object);
  free(v.object);
  free(r.object);
}
int main(int argc, char **argv) {
  check(0, "", "", "5");
  check(1, "registry", "", "5");
  check(1, "registry\nother", "secret", "5");
  check(3, "registry", "", "5");
  if (argc == 2 && !strcmp(argv[1], "--unavailable")) {
    check(0, "https://encore-keyring-test.invalid/index", "", "2");
  } else if (argc == 3 && !strcmp(argv[1], "--roundtrip")) {
    /* Caller supplies a unique disposable namespace, never a real registry. */
    assert(!strncmp(argv[2], "https://encore-keyring-test.invalid/",
                    sizeof("https://encore-keyring-test.invalid/") - 1));
    check(0, argv[2], "", "1");
    check(1, argv[2], "test-secret-one", "0");
    check(0, argv[2], "", "0test-secret-one");
    check(1, argv[2], "test-secret-two", "0");
    check(0, argv[2], "", "0test-secret-two");
    check(2, argv[2], "", "0");
    check(0, argv[2], "", "1");
    check(2, argv[2], "", "0");
  }
  puts("secret-store native tests passed");
}
