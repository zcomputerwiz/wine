/*
 * Mock elevation broker client used by the shell32 shlexec tests to exercise
 * execute_with_broker() (dlls/shell32/shlexec.c). It is loaded in-process by
 * LoadLibraryExW(L"wexhost.dll", ...) and its WexElevateRun entry point simulates
 * a broker approving or denying an elevated launch.
 *
 * Behavior is selected by the WEXHOST_MODE environment variable (read in-process,
 * so the test can set it right before each ShellExecuteEx call):
 *   "deny"  -> return ERROR_CANCELLED without launching anything
 *   (other) -> launch the requested command and report its process handle / pid
 *
 * On every call the received request (image / command line / working directory) is
 * written to the path in the WEXHOST_BINDING_PATH environment variable if set, else to
 * <temp>\wexhost_binding.txt, so the test can verify that the caller's parameters and
 * directory were passed through to the broker.
 *
 * This is a plain Win32 DLL (no Wine-specific APIs), so it can be built with the
 * cross mingw toolchain that a Wine build already uses, per architecture. The matching
 * .def file pins the export name to "WexElevateRun" on both arches:
 *   x86_64-w64-mingw32-gcc -shared -o wexhost.dll wexhost.c wexhost_x86_64.def -static-libgcc
 *   i686-w64-mingw32-gcc   -shared -o wexhost.dll wexhost.c wexhost_i386.def   -static-libgcc
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static void record_request(const WCHAR *image, const WCHAR *cmd, const WCHAR *cwd)
{
    WCHAR path[MAX_PATH], buf[4096];
    DWORD written;
    HANDLE file;

    /* Honor a caller-supplied (process-owned) binding path so the test never writes to a shared fixed name. */
    if (!GetEnvironmentVariableW(L"WEXHOST_BINDING_PATH", path, sizeof(path) / sizeof(WCHAR)))
    {
        if (!GetTempPathW(MAX_PATH, path)) return;
        lstrcatW(path, L"wexhost_binding.txt");
    }

    lstrcpyW(buf, L"image=");
    lstrcatW(buf, image ? image : L"(null)");
    lstrcatW(buf, L"\ncmd=");
    lstrcatW(buf, cmd ? cmd : L"(null)");
    lstrcatW(buf, L"\ncwd=");
    lstrcatW(buf, cwd ? cwd : L"(null)");

    /* Exclusive creation so we never clobber a pre-existing file; fall back to truncating only the file this
     * owned run already created (re-opened on subsequent WexElevateRun calls within the same test). */
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_EXISTS)
        file = CreateFileW(path, GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    WriteFile(file, buf, (DWORD)(lstrlenW(buf) + 1) * sizeof(WCHAR), &written, NULL);
    CloseHandle(file);
}

/* Exported via wexhost_<arch>.def so the entry is named exactly "WexElevateRun" on both arches (i386 stdcall
 * would otherwise decorate it "WexElevateRun@36", which GetProcAddress(..., "WexElevateRun") in shlexec.c
 * cannot find). */
DWORD WINAPI WexElevateRun(const WCHAR *image, const WCHAR *cmd, const WCHAR *cwd,
                           const WCHAR *env, DWORD env_len, DWORD nShow, DWORD flags,
                           HANDLE *process, DWORD *pid)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    WCHAR cmdline[MAX_PATH * 2], mode_buf[16];
    DWORD mode_len;

    record_request(image, cmd, cwd);

    if (process) *process = NULL;
    if (pid) *pid = 0;

    mode_len = GetEnvironmentVariableW(L"WEXHOST_MODE", mode_buf, sizeof(mode_buf) / sizeof(WCHAR));
    if (mode_len && !lstrcmpiW(mode_buf, L"deny"))
        return ERROR_CANCELLED;

    lstrcpyW(cmdline, cmd ? cmd : L"");
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(image, cmdline, NULL, NULL, FALSE, 0, NULL, cwd, &si, &pi))
        return GetLastError();

    if (process) *process = pi.hProcess;
    if (pid) *pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    return TRUE;
}
