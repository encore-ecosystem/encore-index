/* Private transport for plaintext HTTP on IPv4 loopback only. No DNS, proxy,
 * redirects or remote addresses. One monotonic deadline covers the lifetime. */
typedef struct {
#ifdef _WIN32
  SOCKET fd;
#else
  int fd;
#endif
  uint64_t deadline;
  bool failed;
} encore_loopback;

static bool encore_loopback_wait(encore_loopback *stream, bool write) {
  for (;;) {
    uint64_t now = encore_clock_ms(1);
    if (now >= stream->deadline)
      break;
    uint64_t remaining = stream->deadline - now;
#ifdef _WIN32
    fd_set pending;
    FD_ZERO(&pending);
    FD_SET(stream->fd, &pending);
    struct timeval timeout = {(long)(remaining / 1000),
                              (long)((remaining % 1000) * 1000)};
    int result = select(0, write ? NULL : &pending, write ? &pending : NULL,
                        NULL, &timeout);
    if (result > 0)
      return true;
    if (result < 0 && WSAGetLastError() == WSAEINTR)
      continue;
#else
    struct pollfd pending = {stream->fd, write ? POLLOUT : POLLIN, 0};
    int result = poll(&pending, 1, (int)remaining);
    if (result > 0 && !(pending.revents & POLLNVAL))
      return true;
    if (result < 0 && errno == EINTR)
      continue;
#endif
    break;
  }
  stream->failed = true;
  encore_set_net_error_cstr("loopback HTTP deadline or socket failure");
  return false;
}

static bool encore_loopback_again(void) {
#ifdef _WIN32
  int error = WSAGetLastError();
  return error == WSAEWOULDBLOCK || error == WSAEINTR;
#else
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

size_t encore_http_loopback_connect(size_t port, size_t timeout_ms) {
  if (!port || port > 65535 || !timeout_ms || timeout_ms > INT_MAX ||
      !encore_net_init())
    return 0;
  encore_loopback *stream = calloc(1, sizeof(*stream));
  if (!stream)
    return 0;
  stream->deadline = encore_clock_ms(1) + timeout_ms;
  stream->fd = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
  if (stream->fd == INVALID_SOCKET) {
    free(stream);
    return 0;
  }
  u_long mode = 1;
  if (ioctlsocket(stream->fd, FIONBIO, &mode))
    goto fail;
#else
  if (stream->fd < 0) {
    free(stream);
    return 0;
  }
  int flags = fcntl(stream->fd, F_GETFL, 0);
  if (flags < 0 || fcntl(stream->fd, F_SETFL, flags | O_NONBLOCK) < 0)
    goto fail;
#ifdef SO_NOSIGPIPE
  int enabled = 1;
  if (setsockopt(stream->fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                 sizeof(enabled)))
    goto fail;
#endif
#endif
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons((uint16_t)port);
  if (connect(stream->fd, (struct sockaddr *)&address, sizeof(address))) {
#ifdef _WIN32
    if (WSAGetLastError() != WSAEWOULDBLOCK)
      goto fail;
#else
    if (errno != EINPROGRESS)
      goto fail;
#endif
    if (!encore_loopback_wait(stream, true))
      goto fail;
    int error = 0;
#ifdef _WIN32
    int size = sizeof(error);
    if (getsockopt(stream->fd, SOL_SOCKET, SO_ERROR, (char *)&error, &size) ||
        error)
      goto fail;
#else
    socklen_t size = sizeof(error);
    if (getsockopt(stream->fd, SOL_SOCKET, SO_ERROR, &error, &size) || error)
      goto fail;
#endif
  }
  return (size_t)(uintptr_t)stream;
fail:
  encore_close_socket(stream->fd);
  free(stream);
  encore_set_net_error_cstr("loopback HTTP connection failed");
  return 0;
}

encore_str encore_http_loopback_read(size_t handle, size_t max) {
  encore_loopback *stream = (encore_loopback *)(uintptr_t)handle;
  if (!stream || stream->failed || !max)
    return encore_empty_str();
  char buffer[16384];
  if (max > sizeof(buffer))
    max = sizeof(buffer);
  while (encore_loopback_wait(stream, false)) {
    int count = (int)recv(stream->fd, buffer, (int)max, 0);
    if (count == 0)
      return encore_empty_str();
    if (count > 0) {
      char *owned = malloc((size_t)count + 1);
      if (!owned)
        break;
      memcpy(owned, buffer, (size_t)count);
      owned[count] = 0;
      return encore_from_owned_buffer(owned, (size_t)count);
    }
    if (!encore_loopback_again())
      break;
  }
  stream->failed = true;
  encore_set_net_error_cstr("loopback HTTP read failed or deadline exceeded");
  return encore_empty_str();
}

bool encore_http_loopback_failed(size_t handle) {
  encore_loopback *stream = (encore_loopback *)(uintptr_t)handle;
  return !stream || stream->failed;
}

int32_t encore_http_loopback_write(size_t handle, encore_str data) {
  encore_loopback *stream = (encore_loopback *)(uintptr_t)handle;
  if (!stream || stream->failed || encore_str_size(data) > INT_MAX)
    return -1;
  size_t offset = 0, length = encore_str_size(data);
  while (offset < length && encore_loopback_wait(stream, true)) {
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
    int count = (int)send(stream->fd, encore_str_data(data) + offset,
                          (int)(length - offset), flags);
    if (count > 0)
      offset += (size_t)count;
    else if (count == 0 || !encore_loopback_again())
      break;
  }
  if (offset == length)
    return (int32_t)length;
  stream->failed = true;
  encore_set_net_error_cstr("loopback HTTP write failed or deadline exceeded");
  return -1;
}

int32_t encore_http_loopback_close(size_t handle) {
  encore_loopback *stream = (encore_loopback *)(uintptr_t)handle;
  if (!stream)
    return 0;
  int result = encore_close_socket(stream->fd);
  free(stream);
  return result;
}
