#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <mutex>

namespace winvert4
{
    inline const wchar_t* LogFilePath()
    {
        // Always write to the user's temp directory to avoid permission issues
        static wchar_t path[MAX_PATH] = L"";
        static bool initialized = false;
        if (!initialized)
        {
            wchar_t tempDir[MAX_PATH] = L"";
            if (GetTempPathW(MAX_PATH, tempDir) == 0)
            {
                // As a last resort, write to current directory
                lstrcpynW(tempDir, L".", MAX_PATH);
            }
            wsprintfW(path, L"%swinvert4.log", tempDir);
            initialized = true;
        }
        return path;
    }

    inline void Log(const char* line)
    {
        static std::mutex m;
        std::lock_guard<std::mutex> lock(m);

        SYSTEMTIME st{}; GetLocalTime(&st);
        char header[64];
        _snprintf_s(header, _TRUNCATE, "%02u:%02u:%02u:%03u ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

        HANDLE h = CreateFileW(LogFilePath(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD w = 0;
        WriteFile(h, header, (DWORD)strlen(header), &w, nullptr);
        WriteFile(h, line, (DWORD)strlen(line), &w, nullptr);
        static const char crlf[] = "\r\n";
        WriteFile(h, crlf, 2, &w, nullptr);
        CloseHandle(h);
    }

    inline void Logf(const char* fmt, ...)
    {
        char buf[1024];
        va_list args; va_start(args, fmt);
        _vsnprintf_s(buf, _TRUNCATE, fmt, args);
        va_end(args);
        Log(buf);
    }
}
