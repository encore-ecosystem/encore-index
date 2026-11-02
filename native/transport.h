#include <arpa/inet.h>
#ifndef ENCORE_REGISTRY_TRANSPORT_H
#define ENCORE_REGISTRY_TRANSPORT_H
/* SQL-free transport. States: 1 storage queued, 2 storage-owned, 3 complete,
 * 4 validation queued, 5 OAuth queued, 6 OAuth-owned, 7 validator-owned. */
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
  /* Explicit LAN listening permits HTTP origins on private IPv4 networks. */
  bool lan_http = false;
  const char *bind = getenv("ENCORE_INDEX_BIND");
  if (!secure && bind && !strcmp(bind, "0.0.0.0")) {
    char host[64];
    size_t host_length = strcspn(authority, ":");
    if (host_length && host_length < sizeof(host)) {
      memcpy(host, authority, host_length);
      host[host_length] = 0;
      struct in_addr address;
      if (inet_pton(AF_INET, host, &address) == 1) {
        uint32_t ip = ntohl(address.s_addr);
        lan_http = (ip & 0xff000000u) == 0x0a000000u ||
                   (ip & 0xfff00000u) == 0xac100000u ||
                   (ip & 0xffff0000u) == 0xc0a80000u;
      }
    }
  }
  if (!secure && !lan_http && !((!strncmp(authority, "127.0.0.1", 9) &&
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
  return true;
}
static void reg_stop(void) {
  pthread_mutex_lock(&reg_mutex); reg_stopping = true;
  pthread_cond_broadcast(&reg_ready); pthread_mutex_unlock(&reg_mutex);
}
/* All Encore workers must be joined before queue memory is released. */
static void reg_service_stop(void) {
  reg_stop();
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
