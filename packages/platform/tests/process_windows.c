/* Real CreateProcess capture; also runnable under Wine without a compiler bootstrap. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <io.h>
#include "../process_windows.h"

static char *utf8(const wchar_t *wide) {
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, NULL, 0, NULL, NULL);
    char *text = size > 0 ? malloc((size_t)size) : NULL;
    if (text != NULL) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, text, size, NULL, NULL);
    return text;
}

typedef struct { char **argv; char *cwd; int failed; } invocation;
static DWORD WINAPI check_capture(void *raw) {
    invocation *test = raw;
    FILE *capture = encore_windows_capture_file();
    if (capture == NULL) { test->failed = 1; return 0; }
    int32_t status = encore_windows_capture(test->argv, test->cwd, capture);
    rewind(capture);
    char actual[2048] = {0}, expected[2048];
    size_t count = fread(actual, 1, sizeof(actual) - 1, capture);
    fclose(capture);
    snprintf(expected, sizeof(expected), "space arg||quote\"arg|tail\\|caf\xc3\xa9|%s|stderr", test->cwd);
    if (status != 7 || count != strlen(expected) || strcmp(actual, expected)) {
        fprintf(stderr, "capture failed: status=%d, actual='%s', expected='%s'\n", status, actual, expected);
        test->failed = 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "child")) {
        /* A UTF-8-aware child consumes the Unicode Windows command line,
           rather than the legacy ANSI argv supplied by the CRT. */
        int count = 0;
        wchar_t **wide_args = CommandLineToArgvW(GetCommandLineW(), &count);
        if (wide_args == NULL || count != 7) return 99;
        for (int i = 2; i < count; i += 1) {
            char *arg = utf8(wide_args[i]); printf("%s|", arg); free(arg);
        }
        LocalFree(wide_args);
        wchar_t cwd[1024];
        GetCurrentDirectoryW(1024, cwd);
        char *directory = utf8(cwd);
        printf("%s|", directory); free(directory); fflush(stdout);
        fputs("stderr", stderr); fflush(stderr);
        return 7;
    }
    wchar_t executable[1024], original[1024], temporary[1024], directory[1200];
    if (!GetModuleFileNameW(NULL, executable, 1024) || !GetCurrentDirectoryW(1024, original) ||
        !GetTempPathW(1024, temporary)) return 1;
    swprintf(directory, 1200, L"%lsencore process \x03bb %lu", temporary, (unsigned long)GetCurrentProcessId());
    if (!CreateDirectoryW(directory, NULL)) return 2;
    /* Exercise the shared file factory with a Unicode temp path, binary IO,
       delete-on-close and a missing directory, without changing the cwd. */
    wchar_t old_tmp[32768];
    DWORD old_tmp_size = GetEnvironmentVariableW(L"TMP", old_tmp, 32768);
    if (old_tmp_size >= 32768 || !SetEnvironmentVariableW(L"TMP", directory)) return 5;
    FILE *temporary_file = encore_windows_capture_file();
    if (temporary_file == NULL) return 6;
    wchar_t temporary_name[2048];
    DWORD name_size = GetFinalPathNameByHandleW((HANDLE)_get_osfhandle(_fileno(temporary_file)),
        temporary_name, 2048, FILE_NAME_NORMALIZED);
    const char binary[] = "\r\n\0bytes";
    char round_trip[sizeof(binary)];
    if (name_size == 0 || name_size >= 2048 ||
        fwrite(binary, 1, sizeof(binary), temporary_file) != sizeof(binary) ||
        fseek(temporary_file, 0, SEEK_SET) != 0 ||
        fread(round_trip, 1, sizeof(binary), temporary_file) != sizeof(binary) ||
        memcmp(binary, round_trip, sizeof(binary))) return 7;
    fclose(temporary_file);
    if (GetFileAttributesW(temporary_name) != INVALID_FILE_ATTRIBUTES) return 8;
    wchar_t missing_temp[1400];
    swprintf(missing_temp, 1400, L"%ls\\missing-temp-directory", directory);
    if (!SetEnvironmentVariableW(L"TMP", missing_temp)) return 9;
    FILE *unavailable = encore_windows_capture_file();
    if (unavailable != NULL) { fclose(unavailable); return 10; }
    if (!SetEnvironmentVariableW(L"TMP", old_tmp_size == 0 ? NULL : old_tmp)) return 11;
    char *program = utf8(executable), *cwd = utf8(directory);
    char *args[] = {program, "child", "space arg", "", "quote\"arg", "tail\\", "caf\xc3\xa9", NULL};
    invocation tests[4]; HANDLE threads[4];
    for (size_t i = 0; i < 4; i += 1) {
        tests[i] = (invocation){args, cwd, 0};
        threads[i] = CreateThread(NULL, 0, check_capture, &tests[i], 0, NULL);
        if (threads[i] == NULL) return 3;
    }
    WaitForMultipleObjects(4, threads, TRUE, INFINITE);
    int failed = 0;
    for (size_t i = 0; i < 4; i += 1) { CloseHandle(threads[i]); failed |= tests[i].failed; }
    wchar_t after[1024]; GetCurrentDirectoryW(1024, after);
    failed |= wcscmp(original, after) != 0;
    FILE *capture = encore_windows_capture_file();
    if (capture == NULL) return 4;
    failed |= encore_windows_capture(args, "encore-missing-directory", capture) != -1;
    char *missing[] = {"encore-missing-executable", NULL};
    failed |= encore_windows_capture(missing, "", capture) != -1;
    fclose(capture);
    RemoveDirectoryW(directory); free(program); free(cwd);
    if (!failed) puts("Windows process capture passed: Unicode temporary files, binary IO, delete-on-close, argv, Unicode cwd, stderr, exit status, parallel isolation, startup failures");
    return failed;
}
