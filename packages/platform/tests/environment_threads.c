#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

typedef struct { void *object; } encore_str;
size_t encore_os_argc(void);
encore_str encore_os_argv(size_t index);
encore_str encore_from_cstr_copy(const char *value);
char *encore_str_data(encore_str value);
void encore_str_drop(encore_str value);
int32_t encore_net_tcp_connect(encore_str address);
encore_str encore_net_last_error(void);
size_t encore_thread_spawn(void (*entry)(void *), void *payload, void (*cleanup)(void *));
void *encore_thread_join(size_t token);
void encore_thread_release(size_t token);

static atomic_bool start = false;
typedef struct { unsigned index; } worker;

static void exercise(void *payload) {
    worker *self = payload;
    while (!atomic_load_explicit(&start, memory_order_acquire)) {}
    const char *address = self->index % 2 == 0 ? "missing-port" : "localhost:invalid";
    const char *expected = self->index % 2 == 0 ? "invalid addr, expected host:port" : "invalid port";
    encore_str input = encore_from_cstr_copy(address);
    for (unsigned iteration = 0; iteration < 500; ++iteration) {
        assert(encore_os_argc() > 0);
        encore_str name = encore_os_argv(0);
        assert(encore_str_data(name) != NULL && encore_str_data(name)[0] != '\0');
        encore_str_drop(name);
        assert(encore_net_tcp_connect(input) == -1);
        encore_str error = encore_net_last_error();
        assert(strcmp(encore_str_data(error), expected) == 0);
        encore_str_drop(error);
    }
    encore_str_drop(input);
}

int main(void) {
    worker workers[16];
    size_t tasks[16];
    for (unsigned index = 0; index < 16; ++index) {
        workers[index].index = index;
        tasks[index] = encore_thread_spawn(exercise, &workers[index], NULL);
        assert(tasks[index] != 0);
    }
    atomic_store_explicit(&start, true, memory_order_release);
    for (unsigned index = 0; index < 16; ++index) {
        (void)encore_thread_join(tasks[index]);
        encore_thread_release(tasks[index]);
    }
    return 0;
}
