/* Network.framework TLS adapter. Included by runtime.c on macOS.
 * Public operations are synchronous and must not run on the private callback
 * queue. Timed-out operations own their buffers until their callback completes. */
#include <Network/Network.h>
#include <Security/SecProtocolOptions.h>
#include <dispatch/dispatch.h>

typedef struct {
    _Atomic unsigned refs;
    dispatch_semaphore_t done;
    bool failed, complete;
    char *buffer;
    size_t length;
} encore_tls_operation;

static encore_tls_operation *encore_tls_operation_new(void) {
    encore_tls_operation *op = calloc(1, sizeof(*op));
    if (!op) return NULL;
    atomic_init(&op->refs, 2); /* caller and exactly one completion */
    op->done = dispatch_semaphore_create(0);
    if (!op->done) { free(op); return NULL; }
    return op;
}
static void encore_tls_operation_release(encore_tls_operation *op) {
    if (atomic_fetch_sub(&op->refs, 1) == 1) {
        dispatch_release(op->done);
        free(op->buffer);
        free(op);
    }
}
typedef struct {
    _Atomic unsigned refs; /* API owner and final cancellation notification */
    nw_connection_t connection;
    dispatch_queue_t queue;
    dispatch_semaphore_t connected;
    CFStringRef hostname;
    CFArrayRef anchors;
    _Atomic bool ready;
    bool notified; /* callback queue only */
    bool read_failed, eof, poisoned;
    size_t timeout_ms;
} encore_tls_client;

static dispatch_time_t encore_tls_deadline(size_t ms) {
    int64_t ns = ms > (size_t)(INT64_MAX / NSEC_PER_MSEC)
        ? INT64_MAX : (int64_t)ms * NSEC_PER_MSEC;
    return dispatch_time(DISPATCH_TIME_NOW, ns);
}
static CFArrayRef encore_apple_load_ca(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    long size = ftell(file);
    /* Bound untrusted input before allocating. */
    if (size <= 0 || size > 16 * 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file); return NULL;
    }
    UInt8 *bytes = malloc((size_t)size);
    if (!bytes) { fclose(file); return NULL; }
    bool read_ok = fread(bytes, 1, (size_t)size, file) == (size_t)size;
    fclose(file);
    CFDataRef data = read_ok ? CFDataCreate(NULL, bytes, size) : NULL;
    free(bytes);
    if (!data) return NULL;
    SecExternalFormat format = kSecFormatUnknown;
    SecExternalItemType type = kSecItemTypeCertificate;
    CFArrayRef items = NULL;
    OSStatus status = SecItemImport(data, NULL, &format, &type, 0, NULL, NULL, &items);
    CFRelease(data);
    if (status != errSecSuccess || !items || CFArrayGetCount(items) == 0) {
        if (items) CFRelease(items);
        return NULL;
    }
    for (CFIndex i = 0; i < CFArrayGetCount(items); ++i) {
        if (CFGetTypeID(CFArrayGetValueAtIndex(items, i)) != SecCertificateGetTypeID()) {
            CFRelease(items); return NULL;
        }
    }
    return items;
}
static void encore_tls_client_destroy(encore_tls_client *client) {
    if (client->connection) nw_release(client->connection);
    if (client->hostname) CFRelease(client->hostname);
    if (client->anchors) CFRelease(client->anchors);
    dispatch_release(client->connected);
    dispatch_release(client->queue);
    free(client);
}
static void encore_tls_client_release(encore_tls_client *client) {
    if (atomic_fetch_sub(&client->refs, 1) == 1) encore_tls_client_destroy(client);
}
int32_t encore_tls_close(size_t handle) {
    encore_tls_client *client = (encore_tls_client *)(uintptr_t)handle;
    if (!client) return 0;
    nw_connection_cancel(client->connection);
    /* Never wait for callbacks here: a trust evaluation may still be running.
     * The final cancellation notification owns the remaining client reference.
     * Read/write completions only capture independently owned operations. */
    encore_tls_client_release(client);
    return 0;
}
size_t encore_tls_client_connect(encore_str host, size_t port, encore_str ca_file, size_t timeout_ms) {
    char *host_c = encore_to_cstr(host), *ca_c = encore_to_cstr(ca_file);
    if (!host_c || !ca_c || !host_c[0] || strlen(host_c) != encore_str_size(host) ||
        strlen(ca_c) != encore_str_size(ca_file) || port == 0 || port > 65535 || timeout_ms == 0) {
        free(host_c); free(ca_c); encore_set_net_error_cstr("invalid TLS endpoint"); return 0;
    }
    encore_tls_client *client = calloc(1, sizeof(*client));
    if (!client) { free(host_c); free(ca_c); encore_set_net_error_cstr("TLS allocation failed"); return 0; }
    atomic_init(&client->refs, 1);
    client->queue = dispatch_queue_create("org.encore.tls", DISPATCH_QUEUE_SERIAL);
    client->connected = dispatch_semaphore_create(0);
    client->timeout_ms = timeout_ms;
    atomic_init(&client->ready, false);
    client->hostname = CFStringCreateWithCString(NULL, host_c, kCFStringEncodingUTF8);
    if (ca_c[0]) client->anchors = encore_apple_load_ca(ca_c);
    bool valid = client->hostname && (!ca_c[0] || client->anchors);
    free(ca_c);
    if (!valid) {
        free(host_c); encore_tls_client_destroy(client);
        encore_set_net_error_cstr("invalid TLS hostname or CA file"); return 0;
    }
    nw_parameters_t parameters = nw_parameters_create_secure_tcp(
        ^(nw_protocol_options_t tls) {
            sec_protocol_options_t options = nw_tls_copy_sec_protocol_options(tls);
            sec_protocol_options_set_min_tls_protocol_version(options, tls_protocol_version_TLSv12);
            sec_protocol_options_set_tls_server_name(options, host_c);
            sec_protocol_options_set_verify_block(options,
                ^(sec_protocol_metadata_t metadata, sec_trust_t trust, sec_protocol_verify_complete_t complete) {
                    (void)metadata;
                    SecTrustRef peer = sec_trust_copy_ref(trust);
                    SecPolicyRef policy = SecPolicyCreateSSL(true, client->hostname);
                    bool accepted = peer && policy;
                    if (accepted) accepted = SecTrustSetPolicies(peer, policy) == errSecSuccess;
                    if (accepted && client->anchors) {
                        accepted = SecTrustSetAnchorCertificates(peer, client->anchors) == errSecSuccess &&
                            SecTrustSetAnchorCertificatesOnly(peer, false) == errSecSuccess;
                    }
                    CFErrorRef error = NULL;
                    if (accepted) accepted = SecTrustEvaluateWithError(peer, &error);
                    if (error) CFRelease(error);
                    if (policy) CFRelease(policy);
                    if (peer) CFRelease(peer);
                    complete(accepted);
                }, client->queue);
            sec_release(options);
        }, NW_PARAMETERS_DEFAULT_CONFIGURATION);
    char port_c[6];
    snprintf(port_c, sizeof(port_c), "%zu", port);
    nw_endpoint_t endpoint = nw_endpoint_create_host(host_c, port_c);
    free(host_c);
    client->connection = nw_connection_create(endpoint, parameters);
    nw_release(endpoint);
    nw_release(parameters);
    if (!client->connection) {
        encore_tls_client_destroy(client); encore_set_net_error_cstr("TLS connection creation failed"); return 0;
    }
    nw_connection_set_queue(client->connection, client->queue);
    atomic_fetch_add(&client->refs, 1);
    nw_connection_set_state_changed_handler(client->connection,
        ^(nw_connection_state_t state, nw_error_t error) {
            (void)error;
            if (!client->notified && (state == nw_connection_state_ready ||
                state == nw_connection_state_failed || state == nw_connection_state_cancelled)) {
                client->notified = true;
                atomic_store(&client->ready, state == nw_connection_state_ready);
                dispatch_semaphore_signal(client->connected);
            }
            if (state == nw_connection_state_cancelled) encore_tls_client_release(client);
        });
    nw_connection_start(client->connection);
    bool timed_out = dispatch_semaphore_wait(client->connected, encore_tls_deadline(timeout_ms)) != 0;
    if (timed_out || !atomic_load(&client->ready)) {
        encore_tls_close((size_t)(uintptr_t)client);
        encore_set_net_error_cstr(timed_out ? "TLS connect timed out" : "TLS handshake failed"); return 0;
    }
    return (size_t)(uintptr_t)client;
}
static bool encore_tls_wait(encore_tls_client *client, encore_tls_operation *op) {
    if (dispatch_semaphore_wait(op->done, encore_tls_deadline(client->timeout_ms)) != 0) {
        client->poisoned = true;
        nw_connection_cancel(client->connection);
        encore_set_net_error_cstr("TLS operation timed out");
        return false;
    }
    if (op->failed) {
        client->poisoned = true;
        encore_set_net_error_cstr("TLS operation failed");
        return false;
    }
    return true;
}
encore_str encore_tls_read(size_t handle, size_t max) {
    encore_tls_client *client = (encore_tls_client *)(uintptr_t)handle;
    if (!client || max == 0) return encore_empty_str();
    client->read_failed = false;
    if (client->poisoned) { client->read_failed = true; encore_set_net_error_cstr("TLS connection failed"); return encore_empty_str(); }
    if (client->eof) return encore_empty_str();
    encore_tls_operation *op = encore_tls_operation_new();
    if (!op) { client->read_failed = true; encore_set_net_error_cstr("TLS allocation failed"); return encore_empty_str(); }
    uint32_t limit = max > UINT32_MAX ? UINT32_MAX : (uint32_t)max;
    nw_connection_receive(client->connection, 1, limit,
        ^(dispatch_data_t content, nw_content_context_t context, bool complete, nw_error_t error) {
            (void)context;
            op->failed = error != NULL;
            op->complete = complete;
            if (content && !op->failed) {
                const void *bytes = NULL;
                size_t size = 0;
                dispatch_data_t mapped = dispatch_data_create_map(content, &bytes, &size);
                if (mapped && size < SIZE_MAX) {
                    op->buffer = malloc(size + 1);
                    if (op->buffer) { memcpy(op->buffer, bytes, size); op->buffer[size] = 0; op->length = size; }
                    else op->failed = true;
                } else op->failed = true;
                if (mapped) dispatch_release(mapped);
            }
            dispatch_semaphore_signal(op->done);
            encore_tls_operation_release(op);
        });
    encore_str result = encore_empty_str();
    if (encore_tls_wait(client, op)) {
        client->eof = op->complete;
        if (op->buffer) {
            result = encore_from_owned_buffer(op->buffer, op->length);
            op->buffer = NULL;
        }
    } else client->read_failed = true;
    encore_tls_operation_release(op);
    return result;
}
bool encore_tls_read_failed(size_t handle) {
    encore_tls_client *client = (encore_tls_client *)(uintptr_t)handle;
    return client && client->read_failed;
}
int32_t encore_tls_write(size_t handle, encore_str data) {
    encore_tls_client *client = (encore_tls_client *)(uintptr_t)handle;
    if (!client || client->poisoned) return -1;
    size_t length = encore_str_size(data);
    if (!length) return 0;
    encore_tls_operation *op = encore_tls_operation_new();
    if (!op) { encore_set_net_error_cstr("TLS allocation failed"); return -1; }
    /* NULL destructor asks dispatch to copy; Encore's string may die on timeout. */
    dispatch_data_t content = dispatch_data_create(encore_str_data(data), length, client->queue, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    if (!content) {
        encore_tls_operation_release(op); encore_tls_operation_release(op);
        encore_set_net_error_cstr("TLS allocation failed"); return -1;
    }
    nw_connection_send(client->connection, content, NW_CONNECTION_DEFAULT_MESSAGE_CONTEXT, true,
        ^(nw_error_t error) {
            op->failed = error != NULL;
            dispatch_semaphore_signal(op->done);
            encore_tls_operation_release(op);
        });
    dispatch_release(content);
    bool ok = encore_tls_wait(client, op);
    encore_tls_operation_release(op);
    return ok ? (length > INT32_MAX ? INT32_MAX : (int32_t)length) : -1;
}
