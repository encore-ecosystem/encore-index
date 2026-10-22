#define _POSIX_C_SOURCE 200809L
#include "../runtime.c"
#include <assert.h>

struct encore_str_object { size_t length; char bytes[]; };
char *encore_str_data(encore_str value) { return value.object->bytes; }
size_t encore_str_size(encore_str value) { return value.object->length; }
encore_str encore_str_from_buffer(const void *bytes, size_t length) {
  struct encore_str_object *object = malloc(sizeof(*object) + length + 1);
  assert(object); object->length = length;
  memcpy(object->bytes, bytes, length); object->bytes[length] = 0;
  return (encore_str){object};
}
static encore_str text(const char *value) { return encore_str_from_buffer(value, strlen(value)); }

/* SQL-free archive transport contract. Storage authorizes before invoking it. */
int main(int argc, char **argv) {
  assert(argc == 2);
  assert(reg_directory_open(argv[1]));
  assert(!reg_directory_open(argv[1]));
  assert(!reg_service_start(argv[1], "http://example.com"));
  assert(reg_service_start(argv[1], "http://127.0.0.1:8137"));
  assert(reg_safe_path("src/lib.enq"));
  assert(!reg_safe_path("../escape"));
  assert(!reg_safe_path("src/CON.txt"));
  assert(!reg_safe_path("src/a\\b"));

  RegJob job = { .op = 3 };
  reg_file_process(&job, 1);
  assert(job.status == 201);
  RegUpload *upload = &reg_uploads[0];
  assert(upload->state == 1 && upload->owner == 1);
  assert(!reg_upload_find(upload->id, 2));
  char id[65]; strcpy(id, upload->id);
  free(job.result);
  job = (RegJob){ .op = 4, .a = id, .b = "0", .body = (unsigned char *)"abc", .length = 3 };
  reg_file_process(&job, 1);
  assert(job.status == 200 && upload->length == 3);
  reg_file_process(&job, 1);
  assert(job.status == 200 && upload->length == 3);
  job.body = (unsigned char *)"xyz";
  reg_file_process(&job, 1);
  assert(job.status == 409 && upload->length == 3);
  upload->state = 4;
  job.op = 5;
  reg_file_process(&job, 1);
  assert(job.status == 409); /* No concurrent validator reads/writes. */
  job.op = 10;
  reg_file_process(&job, 1);
  assert(job.status == 409 && upload->state == 4);
  upload->state = 1;
  reg_file_process(&job, 1);
  assert(job.status == 200 && upload->state == 0);
  free(job.result);
  encore_str empty = text(""), payload = encore_str_from_buffer("a\0b", 3);
  uintptr_t queued = registry_submit(1, empty, empty, empty, payload);
  assert(queued && registry_storage_wait() == queued);
  assert(registry_storage_op(queued) == 1);
  encore_str copied = registry_storage_field(queued, 3);
  assert(encore_str_size(copied) == 3 && !memcmp(encore_str_data(copied), "a\0b", 3));
  free(copied.object);
  registry_storage_finish(queued, 200, empty);
  assert(registry_job_status(queued) == 200);
  copied = registry_job_take(queued); free(copied.object);
  assert(registry_job_status(queued) == 503);

  queued = registry_submit(21, empty, empty, empty, empty);
  assert(registry_storage_wait() == queued);
  assert(registry_oauth_schedule(queued, empty, empty));
  assert(registry_oauth_wait() == queued);
  registry_oauth_finish(queued, empty);
  assert(registry_storage_wait() == queued && registry_storage_op(queued) == 31);
  registry_storage_finish(queued, 502, empty);
  copied = registry_job_take(queued); free(copied.object);
  for (size_t i = 0; i < REG_JOBS; ++i) assert(registry_submit(1, empty, empty, empty, empty));
  assert(!registry_submit(1, empty, empty, empty, empty));
  free(empty.object); free(payload.object);
  reg_service_stop();
  close(reg_lock_fd); reg_lock_fd = -1;
  puts("SQL-free transport: ownership, paths, retry, cancellation and shutdown passed");
  return 0;
}
