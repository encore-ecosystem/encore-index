#ifndef ENCORE_REGISTRY_TRANSPORT_H
#define ENCORE_REGISTRY_TRANSPORT_H
/* SQL-free transport. States: 1 storage queued, 2 storage-owned, 3 complete,
 * 4 validation queued, 5 OAuth queued, 6 OAuth-owned, 7 validator-owned. */
#include <archive.h>
#include <archive_entry.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static char *reg_format(const char *format, ...) {
  va_list args, copy; va_start(args, format); va_copy(copy, args);
  int length = vsnprintf(NULL, 0, format, copy); va_end(copy);
  char *result = length < 0 ? NULL : malloc((size_t)length + 1);
  if (result) vsnprintf(result, (size_t)length + 1, format, args);
  va_end(args); return result;
}
#define REG_ARCHIVE_LIMIT (4u * 1024u * 1024u)
#define REG_CHUNK_LIMIT 65536u
#define REG_JOBS 32
#define REG_UPLOADS 16
typedef struct {
  char id[65], path[4096], checksum[65];
  int64_t owner;
  size_t length;
  int state;
  time_t created;
  char *manifest;
  char *result;
} RegUpload;
typedef struct {
  uint64_t id;
  int state, op, status;
  int64_t owner;
  char *a, *b, *token;
  unsigned char *body;
  size_t length;
  char *result;
  size_t result_length;
  time_t touched;
} RegJob;
static RegUpload reg_uploads[REG_UPLOADS];
static RegJob reg_jobs[REG_JOBS];
static pthread_mutex_t reg_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t reg_upload_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t reg_ready = PTHREAD_COND_INITIALIZER;
static pthread_t reg_workers[1];
static size_t reg_worker_count;
static bool reg_stopping;
static uint64_t reg_next_id;
static char reg_directory[3072], reg_public_url[2048];
static int reg_lock_fd = -1;
static int reg_storage_ready;

static char *reg_quote(const char *value) {
  size_t n = strlen(value);
  if (n > 1024 * 1024)
    return NULL;
  char *out = malloc(n * 6 + 3);
  if (!out)
    return NULL;
  char *p = out;
  *p++ = '"';
  for (size_t i = 0; i < n; ++i) {
    unsigned char c = (unsigned char)value[i];
    if (c == '"' || c == '\\') {
      *p++ = '\\';
      *p++ = (char)c;
    } else if (c < 32) {
      snprintf(p, 7, "\\u%04x", c);
      p += 6;
    } else
      *p++ = (char)c;
  }
  *p++ = '"';
  *p = 0;
  return out;
}
static void reg_reply(RegJob *j, int status, const char *body) {
  j->status = status;
  free(j->result);
  j->result = strdup(body);
  j->result_length = j->result ? strlen(j->result) : 0;
}
static void reg_error(RegJob *j, int status, const char *error) {
  char *quoted = reg_quote(error);
  char *body = quoted ? reg_format("{\"error\":%s}", quoted) : NULL;
  reg_reply(j, status, body ? body : "{\"error\":\"out of memory\"}");
  free(quoted);
  free(body);
}
static bool reg_hex(const char *s) {
  if (strlen(s) != 64)
    return false;
  for (; *s; ++s)
    if (!(*s >= '0' && *s <= '9') && !(*s >= 'a' && *s <= 'f'))
      return false;
  return true;
}
static bool reg_hash_file(const char *path, char result[65]) {
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    return false;
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  unsigned char buffer[65536], digest[32];
  unsigned int n = 0;
  bool ok = ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
  ssize_t count = 0;
  while (ok && (count = read(fd, buffer, sizeof(buffer))) > 0)
    ok = EVP_DigestUpdate(ctx, buffer, (size_t)count);
  ok = ok && count == 0 && EVP_DigestFinal_ex(ctx, digest, &n) && n == 32;
  close(fd);
  EVP_MD_CTX_free(ctx);
  if (ok)
    for (size_t i = 0; i < 32; ++i)
      snprintf(result + i * 2, 3, "%02x", digest[i]);
  return ok;
}
static bool reg_sync_dir(const char *path) {
  int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0)
    return false;
  bool ok = !fsync(fd);
  close(fd);
  return ok;
}
static void reg_upload_clear(RegUpload *u) {
  if (u->path[0])
    unlink(u->path);
  free(u->manifest);
  free(u->result);
  memset(u, 0, sizeof(*u));
}
static RegUpload *reg_upload_find(const char *id, int64_t owner) {
  for (size_t i = 0; i < REG_UPLOADS; ++i)
    if (reg_uploads[i].state && reg_uploads[i].owner == owner &&
        !strcmp(reg_uploads[i].id, id))
      return &reg_uploads[i];
  return NULL;
}
static bool reg_safe_path(const char *path) {
  if (!path || !*path || *path == '/' || strlen(path) > 1024)
    return false;
  const char *part = path;
  for (const char *p = path;; ++p) {
    if (*p == '\\' || *p == ':' || *p == '?' || *p == '*' || *p == '"' ||
        *p == '<' || *p == '>' || *p == '|' || (unsigned char)*p >= 127 ||
        (*p && (unsigned char)*p < 32))
      return false;
    if (*p == '/' || !*p) {
      size_t len = (size_t)(p - part);
      if (!len && *p)
        return false;
      if ((len == 1 && part[0] == '.') ||
          (len == 2 && part[0] == '.' && part[1] == '.'))
        return false;
      if (len && (part[len - 1] == '.' || part[len - 1] == ' '))
        return false;
      char device[5] = {0};
      size_t base = 0;
      while (base < len && part[base] != '.')
        ++base;
      if (base <= 4) {
        for (size_t i = 0; i < base; ++i)
          device[i] = part[i] >= 'A' && part[i] <= 'Z' ? part[i] + ('a' - 'A')
                                                       : part[i];
        if (!strcmp(device, "con") || !strcmp(device, "prn") ||
            !strcmp(device, "aux") || !strcmp(device, "nul") ||
            (base == 4 &&
             (!strncmp(device, "com", 3) || !strncmp(device, "lpt", 3)) &&
             device[3] >= '1' && device[3] <= '9'))
          return false;
      }
      part = p + 1;
    }
    if (!*p)
      break;
  }
  return true;
}
/* No extraction and no execution. All bytes, including sparse/logical sizes,
 * are bounded; duplicate member names are rejected before publication. */
static char *reg_validate_archive(const char *path, char **error) {
  struct archive *a = archive_read_new();
  struct archive_entry *entry;
  archive_read_support_filter_gzip(a);
  archive_read_support_format_tar(a);
  char *names[4096] = {0};
  bool directories[4096] = {0};
  size_t count = 0, total = 0;
  bool source = false;
  char *documents = strdup("[");
  size_t documents_size = 1;
  bool root = false;
  *error = "invalid archive";
  if (!documents || archive_read_open_filename(a, path, 65536) != ARCHIVE_OK)
    goto bad;
  int status;
  while ((status = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
    if (archive_filter_code(a, 0) != ARCHIVE_FILTER_GZIP) {
      *error = "gzip archive required";
      goto bad;
    }
    const char *name = archive_entry_pathname(entry);
    if (count == 4096 || !reg_safe_path(name) || archive_entry_symlink(entry) ||
        archive_entry_hardlink(entry) || archive_entry_sparse_count(entry) ||
        (archive_entry_perm(entry) & 06000) ||
        (archive_entry_filetype(entry) != AE_IFREG &&
         archive_entry_filetype(entry) != AE_IFDIR)) {
      *error = "unsafe archive member or too many files";
      goto bad;
    }
    size_t canonical_length = strlen(name);
    if (name[canonical_length - 1] == '/')
      --canonical_length;
    char canonical[1025];
    for (size_t i = 0; i < canonical_length; ++i)
      canonical[i] =
          name[i] >= 'A' && name[i] <= 'Z' ? name[i] + ('a' - 'A') : name[i];
    canonical[canonical_length] = 0;
    bool directory = archive_entry_filetype(entry) == AE_IFDIR;
    for (size_t i = 0; i < count; ++i) {
      size_t previous = strlen(names[i]);
      if (!strcmp(canonical, names[i]) ||
          (!directories[i] && canonical_length > previous &&
           !strncmp(canonical, names[i], previous) &&
           canonical[previous] == '/') ||
          (!directory && previous > canonical_length &&
           !strncmp(canonical, names[i], canonical_length) &&
           names[i][canonical_length] == '/')) {
        *error = "duplicate or conflicting archive member";
        goto bad;
      }
    }
    directories[count] = directory;
    names[count] = strdup(canonical);
    if (!names[count++])
      goto bad;
    la_int64_t size = archive_entry_size(entry);
    if (size < 0 || size > 64 * 1024 * 1024 ||
        total + (size_t)size > 64 * 1024 * 1024) {
      *error = "unpacked archive exceeds 64 MiB";
      goto bad;
    }
    total += (size_t)size;
    size_t len = strlen(name);
    bool manifest = !strcmp(name, "encore.toml") ||
                    (len > 12 && !strcmp(name + len - 12, "/encore.toml"));
    if (archive_entry_filetype(entry) == AE_IFREG &&
        !strncmp(name, "src/", 4) && len > 4 && !strcmp(name + len - 4, ".enq"))
      source = true;
    if (archive_entry_filetype(entry) == AE_IFDIR)
      continue;
    char *text =
        manifest && size <= 262144 ? calloc((size_t)size + 1, 1) : NULL;
    if (manifest && !text) {
      *error = "manifest too large";
      goto bad;
    }
    char buffer[65536];
    size_t seen = 0;
    la_ssize_t n;
    while ((n = archive_read_data(a, buffer, sizeof(buffer))) > 0) {
      if (seen + (size_t)n > (size_t)size) {
        free(text);
        goto bad;
      }
      if (text)
        memcpy(text + seen, buffer, (size_t)n);
      seen += (size_t)n;
    }
    if (n < 0 || seen != (size_t)size) {
      free(text);
      goto bad;
    }
    if (text) {
      if (memchr(text, 0, (size_t)size)) {
        free(text);
        *error = "manifest contains NUL";
        goto bad;
      }
      char *qp = reg_quote(name), *qt = reg_quote(text);
      char *item = qp && qt
                       ? reg_format("%s{\"path\":%s,\"source\":%s}",
                                         documents_size > 1 ? "," : "", qp, qt)
                       : NULL;
      free(qp);
      free(qt);
      free(text);
      if (!item)
        goto bad;
      size_t extra = strlen(item);
      if (documents_size + extra > 524288) {
        free(item);
        *error = "too much manifest data";
        goto bad;
      }
      char *next = realloc(documents, documents_size + extra + 2);
      if (!next) {
        free(item);
        goto bad;
      }
      documents = next;
      memcpy(documents + documents_size, item, extra + 1);
      documents_size += extra;
      free(item);
      if (!strcmp(name, "encore.toml"))
        root = true;
    }
  }
  if (status != ARCHIVE_EOF || !root || !source) {
    *error = "archive requires root encore.toml and src/*.enq";
    goto bad;
  }
  documents[documents_size++] = ']';
  documents[documents_size] = 0;
  for (size_t i = 0; i < count; ++i)
    free(names[i]);
  archive_read_free(a);
  return documents;
bad:
  for (size_t i = 0; i < count; ++i)
    free(names[i]);
  free(documents);
  archive_read_free(a);
  return NULL;
}

static void reg_file_process(RegJob *j, int64_t owner) {
  if (j->op == 8) {
    if (!reg_hex(j->a)) { reg_error(j, 404, "archive not found"); return; }
    char path[4096];
    snprintf(path, sizeof(path), "%s/archives/%s.tar.gz", reg_directory, j->a);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 1 ||
        st.st_size > REG_ARCHIVE_LIMIT) {
      if (fd >= 0)
        close(fd);
      reg_error(j, 503, "archive unavailable");
      return;
    }
    j->result = malloc((size_t)st.st_size + 1);
    j->result_length = 0;
    while (j->result && j->result_length < (size_t)st.st_size) {
      ssize_t n = read(fd, j->result + j->result_length,
                       (size_t)st.st_size - j->result_length);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        break;
      j->result_length += (size_t)n;
    }
    close(fd);
    if (!j->result || j->result_length != (size_t)st.st_size)
      reg_error(j, 503, "archive read failed");
    else {
      j->result[j->result_length] = 0;
      j->status = 200;
    }
    return;
  }
  pthread_mutex_lock(&reg_upload_mutex);
  if (j->op == 3) {
    RegUpload *slot = NULL;
    size_t owned = 0;
    for (size_t i = 0; i < REG_UPLOADS; ++i) {
      RegUpload *u = &reg_uploads[i];
      if (u->state && u->state != 4 && time(NULL) - u->created > 900)
        reg_upload_clear(u);
      if (!u->state || u->state == 3) {
        if (!slot)
          slot = u;
      } else if (u->owner == owner)
        ++owned;
    }
    if (!slot || owned >= 4)
      reg_error(j, 429, "upload quota exceeded");
    else {
      reg_upload_clear(slot);
      unsigned char random[32];
      if (RAND_bytes(random, 32) != 1)
        reg_error(j, 503, "random source unavailable");
      else {
        for (size_t i = 0; i < 32; ++i)
          snprintf(slot->id + i * 2, 3, "%02x", random[i]);
        snprintf(slot->path, sizeof(slot->path), "%s/staging/%s", reg_directory,
                 slot->id);
        int fd =
            open(slot->path,
                 O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0)
          reg_error(j, 503, "cannot create upload");
        else {
          close(fd);
          slot->owner = owner;
          slot->state = 1;
          slot->created = time(NULL);
          char body[96];
          snprintf(body, sizeof(body), "{\"upload\":\"%s\"}", slot->id);
          reg_reply(j, 201, body);
        }
      }
    }
  } else {
    RegUpload *u = reg_upload_find(j->a, owner);
    if (!u)
      reg_error(j, 404, "upload not found; restart publication");
    else if (u->state == 4) reg_error(j, 409, "validation is still running");
    else if (j->op == 10) {
      if (u->state == 4)
        reg_error(j, 409, "validation is still running; retry cancellation");
      else {
        reg_upload_clear(u);
        reg_reply(j, 200, "{\"cancelled\":true}");
      }
    } else if (j->op == 4) {
      char *end;
      errno = 0;
      unsigned long long offset = strtoull(j->b, &end, 10);
      if (u->state != 1 || !*j->b || *end || errno ||
          offset > REG_ARCHIVE_LIMIT || !j->length ||
          j->length > REG_CHUNK_LIMIT || offset + j->length > REG_ARCHIVE_LIMIT)
        reg_error(j, 400, "invalid chunk or archive exceeds 4 MiB");
      else {
        int fd = open(u->path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        bool ok = fd >= 0;
        if (offset < u->length) {
          unsigned char previous[REG_CHUNK_LIMIT];
          ok = ok && offset + j->length <= u->length &&
               pread(fd, previous, j->length, (off_t)offset) ==
                   (ssize_t)j->length &&
               !memcmp(previous, j->body, j->length);
        } else if (offset == u->length) {
          size_t written = 0;
          while (ok && written < j->length) {
            ssize_t n = pwrite(fd, j->body + written, j->length - written,
                               (off_t)(offset + written));
            if (n < 0 && errno == EINTR)
              continue;
            if (n <= 0) {
              ok = false;
              break;
            }
            written += (size_t)n;
          }
          if (ok)
            ok = !fsync(fd);
          if (ok)
            u->length += j->length;
          else if (fd >= 0)
            ftruncate(fd, (off_t)u->length);
        } else
          ok = false;
        if (fd >= 0)
          close(fd);
        if (ok)
          reg_reply(j, 200, "{\"accepted\":true}");
        else
          reg_error(j, 409, "chunk offset/content mismatch or write failure");
      }
    } else if (j->op == 5) {
      if (u->state == 3)
        reg_reply(j, 200, u->result ? u->result : "{\"published\":true}");
      else if (!reg_hex(j->b) || !u->length)
        reg_error(j, 400, "checksum required");
      else {
        u->state = 4;
        pthread_mutex_unlock(&reg_upload_mutex);
        if (!reg_hash_file(u->path, u->checksum) || strcmp(u->checksum, j->b))
          reg_error(j, 400, "checksum mismatch");
        else {
          char *error;
          free(u->manifest);
          u->manifest = reg_validate_archive(u->path, &error);
          if (u->manifest)
            reg_reply(j, 200, u->manifest);
          else
            reg_error(j, 400, error);
        }
        pthread_mutex_lock(&reg_upload_mutex);
        u->state = 2;
      }
    } else reg_error(j, 400, "unknown file operation");
  }
  pthread_mutex_unlock(&reg_upload_mutex);
}
static void reg_job_clear(RegJob *j) {
  if(j->a) { OPENSSL_cleanse(j->a,strlen(j->a)); free(j->a); }
  if(j->b) { OPENSSL_cleanse(j->b,strlen(j->b)); free(j->b); }
  if (j->token) {
    OPENSSL_cleanse(j->token, strlen(j->token));
    free(j->token);
  }
  if(j->body) { OPENSSL_cleanse(j->body,j->length); free(j->body); }
  if(j->result) { OPENSSL_cleanse(j->result,j->result_length); free(j->result); }
  memset(j, 0, sizeof(*j));
}

static void *reg_validator(void *unused) {
  (void)unused;
  pthread_mutex_lock(&reg_mutex);
  for (;;) {
    if (reg_stopping) break;
    RegJob *job = NULL;
    for (size_t i = 0; i < REG_JOBS; ++i)
      if (reg_jobs[i].state == 4) { job = &reg_jobs[i]; break; }
    if (!job) { pthread_cond_wait(&reg_ready, &reg_mutex); continue; }
    job->state = 7;
    pthread_mutex_unlock(&reg_mutex);
    reg_file_process(job, job->owner);
    pthread_mutex_lock(&reg_mutex);
    job->state = 3; job->touched = time(NULL);
  }
  pthread_mutex_unlock(&reg_mutex);
  return NULL;
}
static bool reg_service_start(const char *directory, const char *url) {
  if (strlen(directory) >= sizeof(reg_directory) ||
      strlen(url) >= sizeof(reg_public_url) || strchr(url, '"') ||
      strchr(url, '\\') || strchr(url, '\n'))
    return false;
  const char *authority = NULL;
  bool secure = !strncmp(url, "https://", 8);
  if (secure)
    authority = url + 8;
  else if (!strncmp(url, "http://", 7))
    authority = url + 7;
  else
    return false;
  if (!*authority)
    return false;
  for (const char *p = authority; *p; ++p) {
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == ':' ||
          *p == '[' || *p == ']'))
      return false;
  }
  if (!secure && !((!strncmp(authority, "127.0.0.1", 9) &&
                    (!authority[9] || authority[9] == ':')) ||
                   (!strncmp(authority, "localhost", 9) &&
                    (!authority[9] || authority[9] == ':')) ||
                   (!strncmp(authority, "[::1]", 5) &&
                    (!authority[5] || authority[5] == ':'))))
    return false;
  strcpy(reg_directory, directory);
  strcpy(reg_public_url, url);
  reg_stopping = false;
  char path[4096];
  snprintf(path, sizeof(path), "%s/staging", directory);
  if (mkdir(path, 0700) && errno != EEXIST)
    return false;
  struct stat directory_stat;
  if (lstat(path, &directory_stat) || !S_ISDIR(directory_stat.st_mode) ||
      directory_stat.st_uid != geteuid() || (directory_stat.st_mode & 0077))
    return false;
  DIR *staging = opendir(path);
  if (!staging)
    return false;
  struct dirent *entry;
  while ((entry = readdir(staging))) {
    if (reg_hex(entry->d_name))
      unlinkat(dirfd(staging), entry->d_name, 0);
  }
  closedir(staging);
  snprintf(path, sizeof(path), "%s/archives", directory);
  if (mkdir(path, 0700) && errno != EEXIST)
    return false;
  if (lstat(path, &directory_stat) || !S_ISDIR(directory_stat.st_mode) ||
      directory_stat.st_uid != geteuid() || (directory_stat.st_mode & 0077))
    return false;

  reg_storage_ready = 0;
  if (pthread_create(&reg_workers[0], NULL, reg_validator, NULL)) return false;
  reg_worker_count = 1;
  return true;
}
static void reg_stop(void) {
  pthread_mutex_lock(&reg_mutex); reg_stopping = true;
  pthread_cond_broadcast(&reg_ready); pthread_mutex_unlock(&reg_mutex);
}
/* All Encore workers must be joined before queue memory is released. */
static void reg_service_stop(void) {
  reg_stop();
  for (size_t i = 0; i < reg_worker_count; ++i) pthread_join(reg_workers[i], NULL);
  reg_worker_count = 0;
  for (size_t i = 0; i < REG_JOBS; ++i) reg_job_clear(&reg_jobs[i]);
  for (size_t i = 0; i < REG_UPLOADS; ++i) reg_upload_clear(&reg_uploads[i]);
}
static bool reg_directory_open(const char *directory) {
  if (reg_lock_fd >= 0 || !directory || !*directory || strlen(directory) > 3000) return false;
  if (mkdir(directory, 0700) && errno != EEXIST) return false;
  struct stat info;
  if (lstat(directory, &info) || !S_ISDIR(info.st_mode) ||
      info.st_uid != geteuid() || (info.st_mode & 0077)) return false;
  char path[4096];
  snprintf(path, sizeof(path), "%s/service.lock", directory);
  int lock = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (lock < 0) return false;
  if (fstat(lock, &info) || !S_ISREG(info.st_mode) || info.st_uid != geteuid() ||
      (info.st_mode & 0077) || flock(lock, LOCK_EX | LOCK_NB)) { close(lock); return false; }
  snprintf(path, sizeof(path), "%s/registry.sqlite3", directory);
  int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
  bool ok = fd >= 0 && !fstat(fd, &info) && S_ISREG(info.st_mode) &&
            info.st_uid == geteuid() && !(info.st_mode & 0077);
  if (fd >= 0) close(fd);
  if (!ok) { close(lock); return false; }
  reg_lock_fd = lock;
  strcpy(reg_directory, directory);
  return true;
}
#endif
