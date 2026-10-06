/* Small native boundary; passwords never appear in argv, files or diagnostics.
 * Wire result: 0 + value, 1 absent, 2 unavailable, 3 locked, 4 denied,
 * 5 invalid input, 6 backend error. No process-global last-error race. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef struct encore_str_object encore_str_object;
typedef struct {
  encore_str_object *object;
} encore_str;
extern char *encore_str_data(encore_str);
extern size_t encore_str_size(encore_str);
extern encore_str encore_str_from_buffer(const void *, size_t);
static encore_str result(char code, const void *data, size_t size) {
  if (size > 2048)
    return encore_str_from_buffer("6", 1);
  char buffer[2049];
  buffer[0] = code;
  if (size)
    memcpy(buffer + 1, data, size);
  encore_str out = encore_str_from_buffer(buffer, size + 1);
  volatile unsigned char *wipe = (volatile unsigned char *)buffer;
  for (size_t i = 0; i <= size; ++i)
    wipe[i] = 0;
  return out;
}
static int valid(encore_str value, size_t max) {
  size_t n = encore_str_size(value);
  const unsigned char *p = (const unsigned char *)encore_str_data(value);
  if (!n || n > max)
    return 0;
  for (size_t i = 0; i < n; ++i)
    if (p[i] < 32 || p[i] == 127)
      return 0;
  return 1;
}

#if defined(__linux__)
#include <dlfcn.h>
#include <pthread.h>
/* libsecret's opaque APIs are loaded at runtime: no build-time GLib dependency.
 */
static pthread_once_t load_once = PTHREAD_ONCE_INIT;
static void *library, *schema;
static void *(*schema_new)(const char *, int, ...);
static char *(*lookup)(const void *, void *, void **, ...);
static int (*store)(const void *, const char *, const char *, const char *,
                    void *, void **, ...);
static int (*clear)(const void *, void *, void **, ...);
static void (*password_free)(char *);
static void (*error_free)(void *);
static int (*error_matches)(const void *, uint32_t, int);
static uint32_t (*secret_quark)(void), (*io_quark)(void);
static int ready;
static void load_backend(void) {
  library = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
  if (!library)
    return;
#define LOAD(field, name)                                                      \
  do {                                                                         \
    *(void **)(&field) = dlsym(library, name);                                 \
    if (!field)                                                                \
      return;                                                                  \
  } while (0)
  LOAD(schema_new, "secret_schema_new");
  LOAD(lookup, "secret_password_lookup_sync");
  LOAD(store, "secret_password_store_sync");
  LOAD(clear, "secret_password_clear_sync");
  LOAD(password_free, "secret_password_free");
  LOAD(error_free, "g_error_free");
  LOAD(error_matches, "g_error_matches");
  LOAD(secret_quark, "secret_error_get_quark");
  LOAD(io_quark, "g_io_error_quark");
#undef LOAD
  schema = schema_new("org.encore.Credentials", 0, "registry", 0, NULL);
  ready = schema != NULL;
  /* Never unload GType providers; their registrations outlive a call. */
}
static encore_str backend(int op, const char *key, const char *value) {
  pthread_once(&load_once, load_backend);
  if (!ready)
    return result('2', NULL, 0);
  void *error = NULL;
  char *password = NULL;
  int ok = 1;
  if (op == 0)
    password = lookup(schema, NULL, &error, "registry", key, NULL);
  else if (op == 1)
    ok = store(schema, "default", "Encore registry credential", value, NULL,
               &error, "registry", key, NULL);
  else
    ok = clear(schema, NULL, &error, "registry", key, NULL);
  if (error) {
    char status = error_matches(error, secret_quark(), 2) ? '3'
                  : (error_matches(error, io_quark(), 14) ||
                     error_matches(error, io_quark(), 19))
                      ? '4'
                      : '2';
    error_free(error);
    if (password)
      password_free(password);
    return result(status, NULL, 0);
  }
  encore_str out = op == 0 ? result(password ? '0' : '1', password,
                                    password ? strlen(password) : 0)
                           : result(op == 2 || ok ? '0' : '4', NULL, 0);
  if (password)
    password_free(password);
  return out;
}
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
static encore_str backend(int op, const char *key, const char *value) {
  CFStringRef account =
      CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
  if (!account)
    return result('5', NULL, 0);
  CFMutableDictionaryRef query =
      CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                &kCFTypeDictionaryValueCallBacks);
  if (!query) {
    CFRelease(account);
    return result('6', NULL, 0);
  }
  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, CFSTR("org.encore.Credentials"));
  CFDictionarySetValue(query, kSecAttrAccount, account);
  OSStatus status;
  CFTypeRef found = NULL;
  if (op == 0) {
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
    status = SecItemCopyMatching(query, &found);
  } else if (op == 2)
    status = SecItemDelete(query);
  else {
    CFDataRef data =
        CFDataCreate(NULL, (const UInt8 *)value, (CFIndex)strlen(value));
    const void *keys[] = {kSecValueData};
    const void *values[] = {data};
    CFDictionaryRef update =
        data ? CFDictionaryCreate(NULL, keys, values, 1,
                                  &kCFTypeDictionaryKeyCallBacks,
                                  &kCFTypeDictionaryValueCallBacks)
             : NULL;
    if (!update) {
      if (data)
        CFRelease(data);
      CFRelease(query);
      CFRelease(account);
      return result('6', NULL, 0);
    }
    status = SecItemUpdate(query, update);
    if (status == errSecItemNotFound) {
      CFDictionarySetValue(query, kSecValueData, data);
      status = SecItemAdd(query, NULL);
    }
    CFRelease(update);
    CFRelease(data);
  }
  encore_str out =
      status == errSecSuccess
          ? result('0', found ? CFDataGetBytePtr((CFDataRef)found) : NULL,
                   found ? (size_t)CFDataGetLength((CFDataRef)found) : 0)
          : result(
                status == errSecItemNotFound            ? (op == 2 ? '0' : '1')
                : status == errSecInteractionNotAllowed ? '3'
                : (status == errSecUserCanceled || status == errSecAuthFailed)
                    ? '4'
                : status == errSecNotAvailable ? '2'
                                               : '6',
                NULL, 0);
  if (found)
    CFRelease(found);
  CFRelease(query);
  CFRelease(account);
  return out;
}
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <wincred.h>
#include <windows.h>
static encore_str backend(int op, const char *key, const char *value) {
  wchar_t target[1200] = L"Encore.Registry:";
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, key, -1, target + 16,
                           1184))
    return result('5', NULL, 0);
  HMODULE library =
      LoadLibraryExW(L"advapi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!library)
    return result('2', NULL, 0);
  BOOL(WINAPI * read_credential)(LPCWSTR, DWORD, DWORD, PCREDENTIALW *) =
      (void *)GetProcAddress(library, "CredReadW");
  BOOL(WINAPI * write_credential)(PCREDENTIALW, DWORD) =
      (void *)GetProcAddress(library, "CredWriteW");
  BOOL(WINAPI * delete_credential)(LPCWSTR, DWORD, DWORD) =
      (void *)GetProcAddress(library, "CredDeleteW");
  void(WINAPI * free_credential)(PVOID) =
      (void *)GetProcAddress(library, "CredFree");
  if (!read_credential || !write_credential || !delete_credential ||
      !free_credential) {
    FreeLibrary(library);
    return result('2', NULL, 0);
  }
  BOOL ok;
  PCREDENTIALW credential = NULL;
  if (op == 0)
    ok = read_credential(target, CRED_TYPE_GENERIC, 0, &credential);
  else if (op == 2)
    ok = delete_credential(target, CRED_TYPE_GENERIC, 0);
  else {
    CREDENTIALW entry = {0};
    entry.Type = CRED_TYPE_GENERIC;
    entry.TargetName = target;
    entry.CredentialBlobSize = (DWORD)strlen(value);
    entry.CredentialBlob = (LPBYTE)value;
    entry.Persist = CRED_PERSIST_LOCAL_MACHINE;
    entry.UserName = L"Encore";
    ok = write_credential(&entry, 0);
  }
  if (!ok) {
    DWORD error = GetLastError();
    FreeLibrary(library);
    return result(error == ERROR_NOT_FOUND               ? (op == 2 ? '0' : '1')
                  : error == ERROR_NO_SUCH_LOGON_SESSION ? '2'
                  : error == ERROR_ACCESS_DENIED || error == ERROR_CANCELLED
                      ? '4'
                      : '6',
                  NULL, 0);
  }
  encore_str out = result('0', credential ? credential->CredentialBlob : NULL,
                          credential ? credential->CredentialBlobSize : 0);
  if (credential) {
    SecureZeroMemory(credential->CredentialBlob,
                     credential->CredentialBlobSize);
    free_credential(credential);
  }
  FreeLibrary(library);
  return out;
}
#else
static encore_str backend(int op, const char *key, const char *value) {
  (void)op;
  (void)key;
  (void)value;
  return result('2', NULL, 0);
}
#endif

encore_str encore_secret_store(int32_t operation, encore_str key,
                               encore_str value) {
  if (operation < 0 || operation > 2 || !valid(key, 1024) ||
      (operation == 1 && !valid(value, 2048)))
    return result('5', NULL, 0);
  char key_copy[1025], value_copy[2049];
  size_t n = encore_str_size(key),
         m = operation == 1 ? encore_str_size(value) : 0;
  memcpy(key_copy, encore_str_data(key), n);
  key_copy[n] = 0;
  if (m)
    memcpy(value_copy, encore_str_data(value), m);
  value_copy[m] = 0;
  encore_str out = backend(operation, key_copy, value_copy);
  volatile unsigned char *wipe = (volatile unsigned char *)value_copy;
  for (size_t i = 0; i <= m; ++i)
    wipe[i] = 0;
  return out;
}
