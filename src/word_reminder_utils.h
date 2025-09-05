#pragma once

#include <string>
#include <vector>
#include <chrono>
#include "imgui.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace WordReminder
{
    namespace Utils
    {
        // 时间处理函数
        std::string FormatTime(const std::chrono::system_clock::time_point& time);
        std::string TimeUntilNow(const std::chrono::system_clock::time_point& time);
        
        // 字符串处理函数
        std::string EscapeField(const std::string& input);
        std::string UnescapeField(const std::string& input);
        std::vector<std::string> SplitByUnescapedPipe(const std::string& line);
        
#ifdef _WIN32
        // Windows系统函数
        std::wstring Utf8ToWide(const std::string& utf8);
        bool IsSystemDarkMode();
        void ApplyDwmWindowAttributes(HWND hwnd, bool useDark);
        float GetDpiScale(HWND hwnd);
#endif
        
        // UI工具函数
        void DrawCopyableText(const char* id, const std::string& text);
        void DrawCopyableMultiline(const char* id, const std::string& text);
    }
}
