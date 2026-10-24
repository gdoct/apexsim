// See process.h.

#include "process.h"
#include "text.h"

#include <algorithm>

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
    // Read while it runs: output larger than the pipe's buffer would otherwise stall the
    // command until the timeout. A hung command is cut off after the timeout.
    char buf[4096];
    auto drain = [&] {
        DWORD n = 0;
        while (PeekNamedPipe(rd, nullptr, 0, nullptr, &n, nullptr) && n &&
               ReadFile(rd, buf, (DWORD)std::min<size_t>(n, sizeof buf), &n, nullptr) && n)
            out.append(buf, n);
    };
    ULONGLONG start = GetTickCount64();
    while (WaitForSingleObject(pi.hProcess, 20) == WAIT_TIMEOUT)
    {
        drain();
        if (GetTickCount64() - start > timeoutMs) { TerminateProcess(pi.hProcess, 1); break; }
    }
    drain();
    CloseHandle(rd);
    CloseHandle(pi.hProcess);
    return true;
}
