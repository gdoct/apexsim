// See process.h.

#include "process.h"
#include "text.h"

bool RunCapture(const std::wstring& line, std::string& out, unsigned long timeoutMs)
{
    SECURITY_ATTRIBUTES sa = { sizeof sa, nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = { sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = line;
    BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return false; }
    CloseHandle(pi.hThread);
    out.clear();
    // The output is a few bytes, far under the pipe's buffer, so waiting first cannot block it;
    // a hung interpreter is cut off after the timeout.
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
    char buf[256];
    DWORD n = 0;
    while (PeekNamedPipe(rd, nullptr, 0, nullptr, &n, nullptr) && n && ReadFile(rd, buf, sizeof buf, &n, nullptr) && n)
        out.append(buf, n);
    CloseHandle(rd);
    CloseHandle(pi.hProcess);
    return true;
}
