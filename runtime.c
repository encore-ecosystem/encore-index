#define _POSIX_C_SOURCE 200809L
#include "native/transport.h"
typedef struct encore_str_object encore_str_object;
typedef struct {
  encore_str_object *object;
} encore_str;
extern char *encore_str_data(encore_str);
extern size_t encore_str_size(encore_str);
extern encore_str encore_str_from_buffer(const void *, size_t);

static encore_str registry_text(const char *text) {
  return encore_str_from_buffer(text ? text : "", text ? strlen(text) : 0);
}
#include "native/transport_bridge.h"
int32_t registry_store_open(encore_str directory) {
  size_t size = encore_str_size(directory);
  return size && size <= 3000 && !memchr(encore_str_data(directory), 0, size) &&
         reg_directory_open(encore_str_data(directory));
}
void registry_store_close(void) {
  reg_service_stop();
  if (reg_lock_fd >= 0) close(reg_lock_fd);
  reg_lock_fd = -1;
}
int32_t registry_service_start(encore_str url) {
  char directory[3072]; strcpy(directory, reg_directory);
  return reg_lock_fd >= 0 && encore_str_size(url) < sizeof(reg_public_url) &&
         !memchr(encore_str_data(url), 0, encore_str_size(url)) &&
         reg_service_start(directory, encore_str_data(url));
}
/* Build metadata only; provider settings and endpoints live in Encore. */
bool registry_auth_test_build(void) {
#if defined(ENCORE_INDEX_AUTH_TEST) || defined(ENCORE_REGISTRY_AUTH_TEST)
  return true;
#else
  return false;
#endif
}
uintptr_t registry_submit(int32_t op, encore_str a, encore_str b,
                          encore_str token, encore_str body) {
  if (encore_str_size(a) > 1024 || encore_str_size(b) > 1024 ||
      encore_str_size(token) > 128 || encore_str_size(body) > 524288)
    return 0;
  pthread_mutex_lock(&reg_mutex);
  RegJob *j = NULL;
  for (size_t i = 0; i < REG_JOBS; ++i) {
    if (reg_jobs[i].state == 3 && time(NULL) - reg_jobs[i].touched > 60)
      reg_job_clear(&reg_jobs[i]);
    if (!reg_jobs[i].state && !j)
      j = &reg_jobs[i];
  }
  uint64_t id = 0;
  if (j && !reg_stopping) {
    j->a = strndup(encore_str_data(a), encore_str_size(a));
    j->b = strndup(encore_str_data(b), encore_str_size(b));
    j->token = strndup(encore_str_data(token), encore_str_size(token));
    j->length = encore_str_size(body);
    j->body = malloc(j->length + 1);
    if (!j->a || !j->b || !j->token || !j->body)
      reg_job_clear(j);
    else {
      memcpy(j->body, encore_str_data(body), j->length);
      j->body[j->length] = 0;
      j->id = ++reg_next_id;
      id = j->id;
      j->op = op;
      j->state = 1;
      j->touched = time(NULL);
      pthread_cond_broadcast(&reg_ready);
    }
  }
  pthread_mutex_unlock(&reg_mutex);
  return (uintptr_t)id;
}
int32_t registry_job_status(uintptr_t id) {
  int status = 503;
  pthread_mutex_lock(&reg_mutex);
  for (size_t i = 0; i < REG_JOBS; ++i)
    if (reg_jobs[i].id == id && id) {
      status = reg_jobs[i].state == 3 ? reg_jobs[i].status : 0;
      break;
    }
  pthread_mutex_unlock(&reg_mutex);
  return status;
}
encore_str registry_job_take(uintptr_t id) {
  pthread_mutex_lock(&reg_mutex);
  for (size_t i = 0; i < REG_JOBS; ++i)
    if (reg_jobs[i].id == id && id && reg_jobs[i].state == 3) {
      RegJob *j = &reg_jobs[i];
      encore_str result =
          encore_str_from_buffer(j->result ? j->result : "", j->result_length);
      reg_job_clear(j);
      pthread_mutex_unlock(&reg_mutex);
      return result;
    }
  pthread_mutex_unlock(&reg_mutex);
  return registry_text("");
}
