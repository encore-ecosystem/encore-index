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
/* Primitive crypto boundary for the Encore storage implementation. */
encore_str registry_crypto_digest(encore_str input) {
  unsigned char bytes[32]; unsigned int n = 0; char hex[65];
  if (EVP_Digest(encore_str_data(input), encore_str_size(input), bytes, &n,
                 EVP_sha256(), NULL) != 1 || n != 32) return registry_text("");
  for (size_t i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", bytes[i]);
  encore_str result = registry_text(hex);
  OPENSSL_cleanse(bytes, sizeof(bytes)); OPENSSL_cleanse(hex, sizeof(hex));
  return result;
}
encore_str registry_crypto_secret(encore_str prefix) {
  if (encore_str_size(prefix) != 4) return registry_text("");
  unsigned char bytes[32]; char text[69];
  if (RAND_bytes(bytes, sizeof(bytes)) != 1) return registry_text("");
  memcpy(text, encore_str_data(prefix), 4);
  for (size_t i = 0; i < 32; ++i) snprintf(text + 4 + i * 2, 3, "%02x", bytes[i]);
  encore_str result = registry_text(text);
  OPENSSL_cleanse(bytes, sizeof(bytes)); OPENSSL_cleanse(text, sizeof(text));
  return result;
}
encore_str registry_crypto_user_code(void) {
  unsigned char bytes[8]; char code[8];
  if (RAND_bytes(bytes, sizeof(bytes)) != 1) return registry_text("");
  for (size_t i = 0; i < 8; ++i) code[i] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"[bytes[i] & 31];
  encore_str result = encore_str_from_buffer(code, sizeof(code));
  OPENSSL_cleanse(bytes, sizeof(bytes)); OPENSSL_cleanse(code, sizeof(code));
  return result;
}
int64_t registry_clock_seconds(void) { return (int64_t)time(NULL); }
bool registry_crypto_equal(encore_str left, encore_str right) {
  size_t size = encore_str_size(left);
  return size == encore_str_size(right) && size == 64 &&
         CRYPTO_memcmp(encore_str_data(left), encore_str_data(right), size) == 0;
}
encore_str registry_crypto_pkce(encore_str verifier) {
  unsigned char hash[32], encoded[45]; unsigned int size = 0;
  if (EVP_Digest(encore_str_data(verifier), encore_str_size(verifier), hash, &size,
                 EVP_sha256(), NULL) != 1 || size != 32) return registry_text("");
  EVP_EncodeBlock(encoded, hash, 32);
  for (size_t i = 0; i < 43; ++i) {
    if (encoded[i] == '+') encoded[i] = '-';
    if (encoded[i] == '/') encoded[i] = '_';
  }
  encore_str result = encore_str_from_buffer(encoded, 43);
  OPENSSL_cleanse(hash, sizeof(hash)); OPENSSL_cleanse(encoded, sizeof(encoded));
  return result;
}
static int query_hex(unsigned char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
encore_str registry_query_decode(encore_str value) {
  size_t n = encore_str_size(value), used = 0;
  const unsigned char *input = (unsigned char *)encore_str_data(value);
  if (n > 8192) return registry_text("");
  char output[8193];
  for (size_t i = 0; i < n; ++i) {
    unsigned char c = input[i];
    if (c == '+') c = ' ';
    else if (c == '%') {
      if (i + 2 >= n) return registry_text("");
      int high = query_hex(input[i + 1]), low = query_hex(input[i + 2]);
      if (high < 0 || low < 0) return registry_text("");
      c = (unsigned char)(high * 16 + low); i += 2;
    }
    if (c < 32 || c == 127) return registry_text("");
    output[used++] = (char)c;
  }
  return encore_str_from_buffer(output, used);
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
static const char *reg_setting(const char *name, const char *legacy) {
  const char *value = getenv(name);
  if (value && *value) return value;
  value = getenv(legacy);
  return value && *value ? value : "";
}
static bool reg_auth_enabled(void) {
  const char *client = reg_setting("ENCORE_INDEX_GITHUB_CLIENT_ID", "ENCORE_GITHUB_CLIENT_ID"), *secret = reg_setting("ENCORE_INDEX_GITHUB_CLIENT_SECRET", "ENCORE_GITHUB_CLIENT_SECRET");
  return client && *client && strlen(client) <= 128 && secret && *secret && strlen(secret) <= 256;
}
int32_t registry_auth_enabled(void) { return reg_auth_enabled(); }
encore_str registry_cookie_name(bool oauth) {
  bool secure = !strncmp(reg_public_url, "https://", 8);
  return registry_text(oauth ? (secure ? "__Host-encore_oauth" : "encore_oauth") :
                              (secure ? "__Host-encore_session" : "encore_session"));
}
encore_str registry_oauth_authorize_url(void) {
#if defined(ENCORE_INDEX_AUTH_TEST) || defined(ENCORE_REGISTRY_AUTH_TEST)
  const char *url = getenv("ENCORE_TEST_AUTHORIZE_URL");
  if (url) return registry_text(url);
#endif
  return registry_text("https://github.com/login/oauth/authorize");
}
encore_str registry_oauth_endpoint(int32_t kind) {
#if defined(ENCORE_INDEX_AUTH_TEST) || defined(ENCORE_REGISTRY_AUTH_TEST)
  const char *value = getenv(kind == 0 ? "ENCORE_TEST_TOKEN_URL" : kind == 1 ? "ENCORE_TEST_USER_URL" : "ENCORE_TEST_CA");
  if (value) return registry_text(value);
#endif
  return registry_text(kind == 0 ? "https://github.com/login/oauth/access_token" :
                       kind == 1 ? "https://api.github.com/user" : "");
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
