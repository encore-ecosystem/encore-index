#ifndef ENCORE_REGISTRY_TRANSPORT_BRIDGE_H
#define ENCORE_REGISTRY_TRANSPORT_BRIDGE_H
/* Included after the Encore string ABI and SQL-free transport definitions. */
static RegJob *reg_owned_job(uintptr_t id, int state) {
  for (size_t i = 0; i < REG_JOBS; ++i)
    if (id && reg_jobs[i].id == id && reg_jobs[i].state == state) return &reg_jobs[i];
  return NULL;
}
static uintptr_t reg_wait(int state, int claimed) {
  pthread_mutex_lock(&reg_mutex);
  for (;;) {
    if (reg_stopping) { pthread_mutex_unlock(&reg_mutex); return 0; }
    for (size_t i = 0; i < REG_JOBS; ++i) {
      RegJob *job = &reg_jobs[i];
      if (job->state == state) {
        job->state = claimed; uintptr_t id = job->id;
        pthread_mutex_unlock(&reg_mutex); return id;
      }
    }
    pthread_cond_wait(&reg_ready, &reg_mutex);
  }
}
uintptr_t registry_storage_wait(void) { return reg_wait(1, 2); }
uintptr_t registry_oauth_wait(void) { return reg_wait(5, 6); }
uintptr_t registry_archive_wait(void) { return reg_wait(4, 7); }
void registry_oauth_stop(void) { reg_stop(); }
void registry_storage_ready(int32_t ok) {
  pthread_mutex_lock(&reg_mutex); reg_storage_ready = ok ? 1 : -1;
  pthread_cond_broadcast(&reg_ready); pthread_mutex_unlock(&reg_mutex);
}
int32_t registry_storage_started(void) {
  pthread_mutex_lock(&reg_mutex);
  while (!reg_storage_ready && !reg_stopping) pthread_cond_wait(&reg_ready, &reg_mutex);
  int result = reg_storage_ready == 1;
  pthread_mutex_unlock(&reg_mutex); return result;
}
int32_t registry_storage_op(uintptr_t id) {
  pthread_mutex_lock(&reg_mutex);
  RegJob *job = reg_owned_job(id, 2); int op = job ? job->op : 0;
  pthread_mutex_unlock(&reg_mutex); return op;
}
encore_str registry_storage_field(uintptr_t id, int32_t field) {
  pthread_mutex_lock(&reg_mutex);
  RegJob *job = reg_owned_job(id, 2);
  const char *text = job ? (field == 0 ? job->a : field == 1 ? job->b : field == 2 ? job->token : (char *)job->body) : "";
  size_t size = job && field == 3 ? job->length : text ? strlen(text) : 0;
  encore_str result = encore_str_from_buffer(text ? text : "", size);
  pthread_mutex_unlock(&reg_mutex); return result;
}
static void reg_complete(RegJob *job) {
  pthread_mutex_lock(&reg_mutex); job->state = 3; job->touched = time(NULL);
  pthread_mutex_unlock(&reg_mutex);
}
encore_str registry_archive_path(uintptr_t id) {
  pthread_mutex_lock(&reg_mutex); RegJob *job = reg_owned_job(id, 7);
  pthread_mutex_unlock(&reg_mutex);
  if (!job) return registry_text("");
  pthread_mutex_lock(&reg_upload_mutex);
  RegUpload *upload = reg_upload_find(job->a, job->owner);
  encore_str path = registry_text("");
  if (!upload) reg_error(job, 404, "upload not found; restart publication");
  else if (upload->state == 3) reg_reply(job, 200, upload->result ? upload->result : "{\"published\":true}");
  else if (upload->state == 4) reg_error(job, 409, "validation is still running");
  else if (!reg_hex(job->b) || !upload->length) reg_error(job, 400, "checksum required");
  else {
    upload->state = 4;
    free(upload->manifest); upload->manifest = NULL;
    pthread_mutex_unlock(&reg_upload_mutex);
    bool ok = reg_hash_file(upload->path, upload->checksum) && !strcmp(upload->checksum, job->b);
    pthread_mutex_lock(&reg_upload_mutex);
    if (ok) path = registry_text(upload->path);
    else { upload->state = 2; reg_error(job, 400, "checksum mismatch"); }
  }
  pthread_mutex_unlock(&reg_upload_mutex);
  return path;
}
void registry_archive_finish(uintptr_t id, encore_str documents, encore_str error) {
  pthread_mutex_lock(&reg_mutex); RegJob *job = reg_owned_job(id, 7);
  pthread_mutex_unlock(&reg_mutex);
  if (!job) return;
  pthread_mutex_lock(&reg_upload_mutex);
  RegUpload *upload = reg_upload_find(job->a, job->owner);
  if (upload && upload->state == 4 && (encore_str_size(documents) || encore_str_size(error))) {
    if (encore_str_size(error)) reg_error(job, 400, encore_str_data(error));
    else if (encore_str_size(documents) && encore_str_size(documents) <= 524288) {
      upload->manifest = strndup(encore_str_data(documents), encore_str_size(documents));
      if (upload->manifest) reg_reply(job, 200, upload->manifest);
      else reg_error(job, 503, "out of memory");
    } else reg_error(job, 400, "invalid archive");
    upload->state = 2;
  }
  pthread_mutex_unlock(&reg_upload_mutex);
  reg_complete(job);
}
void registry_storage_finish(uintptr_t id, int32_t status, encore_str body) {
  pthread_mutex_lock(&reg_mutex); RegJob *job = reg_owned_job(id, 2);
  if (job) {
    reg_reply(job, status, encore_str_data(body));
    job->state = 3; job->touched = time(NULL);
  }
  pthread_mutex_unlock(&reg_mutex);
}
void registry_storage_files(uintptr_t id, int64_t owner) {
  pthread_mutex_lock(&reg_mutex); RegJob *job = reg_owned_job(id, 2);
  if (job && job->op == 5) {
    job->owner = owner; job->state = 4;
    pthread_cond_broadcast(&reg_ready); pthread_mutex_unlock(&reg_mutex); return;
  }
  pthread_mutex_unlock(&reg_mutex);
  if (job) { reg_file_process(job, owner); reg_complete(job); }
}
encore_str registry_storage_upload(uintptr_t id, int64_t owner) {
  pthread_mutex_lock(&reg_mutex); RegJob *job = reg_owned_job(id, 2);
  pthread_mutex_unlock(&reg_mutex);
  if (!job) return registry_text("");
  pthread_mutex_lock(&reg_upload_mutex);
  RegUpload *upload = reg_upload_find(job->a, owner);
  char *json = NULL;
  if (upload && upload->state == 2 && upload->manifest) {
    char *quoted = reg_quote(upload->manifest);
    if (quoted) json = reg_format("{\"checksum\":\"%s\",\"size\":%zu,\"manifests\":%s}", upload->checksum, upload->length, quoted);
    free(quoted);
  } else if (upload && upload->state == 3) json = strdup("{\"existing\":true}");
  encore_str result = registry_text(json); free(json);
  pthread_mutex_unlock(&reg_upload_mutex); return result;
}
bool registry_archive_install(uintptr_t id, int64_t owner, encore_str checksum) {
  pthread_mutex_lock(&reg_mutex); RegJob *job = reg_owned_job(id, 2);
  pthread_mutex_unlock(&reg_mutex);
  if (!job || encore_str_size(checksum) != 64) return false;
  pthread_mutex_lock(&reg_upload_mutex);
  RegUpload *upload = reg_upload_find(job->a, owner);
  bool ok = upload && upload->state == 2 && upload->manifest &&
            !strcmp(upload->checksum, encore_str_data(checksum)) && reg_hex(upload->checksum);
  if (ok) {
    char path[4096], directory[4096], actual[65];
    snprintf(directory, sizeof(directory), "%s/archives", reg_directory);
    snprintf(path, sizeof(path), "%s/archives/%s.tar.gz", reg_directory, upload->checksum);
    ok = (!link(upload->path, path) || errno == EEXIST) && reg_hash_file(path, actual) &&
         !strcmp(actual, upload->checksum) && reg_sync_dir(directory);
  }
  pthread_mutex_unlock(&reg_upload_mutex); return ok;
}
void registry_storage_publish_finish(uintptr_t id, int64_t owner, int32_t status, encore_str body) {
  pthread_mutex_lock(&reg_mutex); RegJob *job = reg_owned_job(id, 2);
  pthread_mutex_unlock(&reg_mutex);
  if (!job) return;
  pthread_mutex_lock(&reg_upload_mutex);
  RegUpload *upload = reg_upload_find(job->a, owner);
  if (upload && (status == 200 || status == 201)) {
    upload->state = 3; free(upload->result); upload->result = strdup(encore_str_data(body));
    unlink(upload->path);
  }
  pthread_mutex_unlock(&reg_upload_mutex);
  registry_storage_finish(id, status, body);
}
bool registry_oauth_schedule(uintptr_t id, encore_str input, encore_str destination) {
  char *body = strndup(encore_str_data(input), encore_str_size(input));
  char *target = strndup(encore_str_data(destination), encore_str_size(destination));
  if (!body || !target) { free(body); free(target); return false; }
  pthread_mutex_lock(&reg_mutex); RegJob *job = reg_owned_job(id, 2);
  if (job) {
    OPENSSL_cleanse(job->body, job->length); free(job->body);
    OPENSSL_cleanse(job->a, strlen(job->a)); free(job->a);
    job->body = (unsigned char *)body; job->length = encore_str_size(input);
    job->a = target; job->state = 5; pthread_cond_broadcast(&reg_ready);
  } else { free(body); free(target); }
  pthread_mutex_unlock(&reg_mutex); return job != NULL;
}
encore_str registry_oauth_input(uintptr_t id) {
  pthread_mutex_lock(&reg_mutex); RegJob *job = reg_owned_job(id, 6);
  encore_str input = job ? encore_str_from_buffer(job->body, job->length) : registry_text("");
  pthread_mutex_unlock(&reg_mutex); return input;
}
void registry_oauth_finish(uintptr_t id, encore_str identity) {
  pthread_mutex_lock(&reg_mutex); RegJob *job = reg_owned_job(id, 6);
  if (job) {
    char *subject = strndup(encore_str_data(identity), encore_str_size(identity));
    if (!subject) { reg_error(job, 503, "out of memory"); job->state = 3; job->touched = time(NULL); }
    else {
      OPENSSL_cleanse(job->b, strlen(job->b)); free(job->b); job->b = subject;
      OPENSSL_cleanse(job->body, job->length); job->length = 0; job->body[0] = 0;
      job->op = 31; job->state = 1; pthread_cond_broadcast(&reg_ready);
    }
  }
  pthread_mutex_unlock(&reg_mutex);
}
#endif
