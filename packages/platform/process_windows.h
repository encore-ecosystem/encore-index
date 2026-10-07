/* Private child handles and cwd: never mutate the parent's CRT descriptors. */
#include <fcntl.h>
#include <stdatomic.h>
#include <wchar.h>

/* CREATE_NEW reserves the name atomically; DELETE_ON_CLOSE also covers errors
   and abnormal process termination. Unlike CRT tmpfile[_s], this uses the
   user's temp directory and does not require writing to the drive root. */
static FILE *encore_windows_capture_file(void) {
    DWORD required = GetTempPathW(0, NULL);
    if (required == 0 || required > UINT32_MAX - 80) return NULL;
    size_t capacity = (size_t)required + 80;
    wchar_t *path = calloc(capacity, sizeof(wchar_t));
    if (path == NULL) return NULL;
    DWORD length = GetTempPathW(required, path);
    if (length == 0 || length >= required) { free(path); return NULL; }
    static _Atomic unsigned long long sequence = 0;
    HANDLE handle = INVALID_HANDLE_VALUE;
    for (unsigned int attempt = 0; attempt < 256; attempt += 1) {
        unsigned long long nonce = atomic_fetch_add(&sequence, 1);
        int written = swprintf(path + length, capacity - length,
            L"encore-capture-%lu-%llu-%llu.tmp", (unsigned long)GetCurrentProcessId(),
            (unsigned long long)GetTickCount64(), nonce);
        if (written < 0 || (size_t)written >= capacity - length) break;
        handle = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
        if (handle != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) break;
    }
    free(path);
    if (handle == INVALID_HANDLE_VALUE) return NULL;
    int descriptor = _open_osfhandle((intptr_t)handle, _O_RDWR | _O_BINARY);
    if (descriptor < 0) { CloseHandle(handle); return NULL; }
    FILE *stream = _fdopen(descriptor, "w+b");
    if (stream == NULL) _close(descriptor);
    return stream;
}

static wchar_t *encore_process_utf16(const char *text) {
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (size <= 0) return NULL;
    wchar_t *wide = malloc((size_t)size * sizeof(wchar_t));
    if (wide != NULL && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, size)) {
        free(wide); return NULL;
    }
    return wide;
}

static int32_t encore_windows_capture(char *const *argv, const char *cwd, FILE *capture) {
    size_t capacity = 1;
    for (size_t index = 0; argv[index] != NULL; index += 1) {
        size_t length = strlen(argv[index]);
        if (capacity > SIZE_MAX - 4 || length > (SIZE_MAX - capacity - 4) / 2) return -1;
        capacity += length * 2 + 4;
    }
    char *quoted = malloc(capacity);
    if (quoted == NULL) return -1;
    size_t used = 0;
    for (size_t index = 0; argv[index] != NULL; index += 1) {
        if (index != 0) quoted[used++] = ' ';
        quoted[used++] = '"';
        size_t slashes = 0;
        for (const char *cursor = argv[index]; ; cursor += 1) {
            if (*cursor == '\\') { slashes += 1; continue; }
            size_t count = (*cursor == '"' || *cursor == '\0') ? slashes * 2 : slashes;
            if (*cursor == '"') count += 1;
            while (count > 0) { quoted[used++] = '\\'; count -= 1; }
            slashes = 0;
            if (*cursor == '\0') break;
            quoted[used++] = *cursor;
        }
        quoted[used++] = '"';
    }
    quoted[used] = '\0';
    wchar_t *command = encore_process_utf16(quoted);
    wchar_t *directory = cwd[0] == '\0' ? NULL : encore_process_utf16(cwd);
    free(quoted);
    int32_t result = -1;
    HANDLE output = INVALID_HANDLE_VALUE, input = INVALID_HANDLE_VALUE;
    STARTUPINFOEXW startup;
    memset(&startup, 0, sizeof(startup));
    startup.StartupInfo.cb = sizeof(startup);
    if (command == NULL || (cwd[0] != '\0' && directory == NULL)) goto cleanup;
    HANDLE source = (HANDLE)_get_osfhandle(_fileno(capture));
    if (source == INVALID_HANDLE_VALUE || !DuplicateHandle(GetCurrentProcess(), source,
        GetCurrentProcess(), &output, 0, TRUE, DUPLICATE_SAME_ACCESS)) goto cleanup;
    HANDLE parent_input = GetStdHandle(STD_INPUT_HANDLE);
    if (parent_input != NULL && parent_input != INVALID_HANDLE_VALUE) {
        if (!DuplicateHandle(GetCurrentProcess(), parent_input, GetCurrentProcess(),
            &input, 0, TRUE, DUPLICATE_SAME_ACCESS)) goto cleanup;
    } else {
        SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
        input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (input == INVALID_HANDLE_VALUE) goto cleanup;
    SIZE_T attributes_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attributes_size);
    startup.lpAttributeList = malloc(attributes_size);
    if (startup.lpAttributeList == NULL) goto cleanup;
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributes_size)) {
        free(startup.lpAttributeList); startup.lpAttributeList = NULL; goto cleanup;
    }
    HANDLE inherited[] = {output, input};
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited, sizeof(inherited), NULL, NULL)) goto cleanup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input;
    startup.StartupInfo.hStdOutput = output;
    startup.StartupInfo.hStdError = output;
    PROCESS_INFORMATION process;
    memset(&process, 0, sizeof(process));
    if (CreateProcessW(NULL, command, NULL, NULL, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, NULL, directory,
        &startup.StartupInfo, &process)) {
        DWORD status = 0;
        if (WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0 &&
            GetExitCodeProcess(process.hProcess, &status) && status <= INT32_MAX) result = (int32_t)status;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
cleanup:
    if (startup.lpAttributeList != NULL) {
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        free(startup.lpAttributeList);
    }
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    free(command); free(directory);
    return result;
}
