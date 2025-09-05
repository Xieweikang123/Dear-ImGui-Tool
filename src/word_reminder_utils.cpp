#include "word_reminder_utils.h"
#include <sstream>
#include <iomanip>
#include <ctime>
#include <algorithm>

#ifdef _WIN32
#include <shellscalingapi.h>
#include <dwmapi.h>
#include <winreg.h>
#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "Shcore.lib")
#endif

namespace WordReminder
{
    namespace Utils
    {
        // 时间格式化函数
        std::string FormatTime(const std::chrono::system_clock::time_point& time)
        {
            auto time_t = std::chrono::system_clock::to_time_t(time);
            std::tm tm = *std::localtime(&time_t);
            std::ostringstream oss;
            oss << std::put_time(&tm, "%H:%M:%S");
            return oss.str();
        }
        
        // 计算距离现在的时间
        std::string TimeUntilNow(const std::chrono::system_clock::time_point& time)
        {
            auto now = std::chrono::system_clock::now();
            auto diff = std::chrono::duration_cast<std::chrono::seconds>(time - now);
            
            if (diff.count() < 0)
            {
                return "已过期 " + std::to_string(-diff.count()) + " 秒";
            }
            else if (diff.count() < 60)
            {
                return std::to_string(diff.count()) + " 秒后";
            }
            else if (diff.count() < 3600)
            {
                auto minutes = diff.count() / 60;
                auto seconds = diff.count() % 60;
                return std::to_string(minutes) + " 分 " + std::to_string(seconds) + " 秒后";
            }
            else
            {
                auto hours = diff.count() / 3600;
                auto minutes = (diff.count() % 3600) / 60;
                auto seconds = diff.count() % 60;
                return std::to_string(hours) + " 小时 " + std::to_string(minutes) + " 分 " + std::to_string(seconds) + " 秒后";
            }
        }
        
        // 对字段进行转义与反转义，避免分隔符与换行破坏一行一条记录的约定
        std::string EscapeField(const std::string& input)
        {
            std::string out;
            out.reserve(input.size());
            for (char ch : input)
            {
                switch (ch)
                {
                    case '\\': out += "\\\\"; break; // 反斜杠
                    case '|':   out += "\\|";   break; // 竖线分隔符
                    case '\n': out += "\\n";   break; // 换行
                    case '\r': out += "\\r";   break; // 回车
                    default:    out += ch;        break;
                }
            }
            return out;
        }

        std::string UnescapeField(const std::string& input)
        {
            std::string out;
            out.reserve(input.size());
            bool esc = false;
            for (size_t i = 0; i < input.size(); ++i)
            {
                char ch = input[i];
                if (!esc)
                {
                    if (ch == '\\')
                    {
                        esc = true;
                        continue;
                    }
                    out += ch;
                }
                else
                {
                    switch (ch)
                    {
                        case 'n': out += '\n'; break;
                        case 'r': out += '\r'; break;
                        case '|': out += '|';  break;
                        case '\\': out += '\\'; break;
                        default:
                            // 未知转义，按字面保留
                            out += ch;
                            break;
                    }
                    esc = false;
                }
            }
            // 如果末尾是孤立的反斜杠，则保留一个反斜杠
            if (esc) out += '\\';
            return out;
        }

        // 将一行按未被转义的 '|' 进行分割
        std::vector<std::string> SplitByUnescapedPipe(const std::string& line)
        {
            std::vector<std::string> parts;
            std::string current;
            bool esc = false;
            for (char ch : line)
            {
                if (!esc)
                {
                    if (ch == '\\')
                    {
                        esc = true;
                        current.push_back(ch); // 保留反斜杠，供反转义阶段处理
                    }
                    else if (ch == '|')
                    {
                        parts.push_back(current);
                        current.clear();
                    }
                    else
                    {
                        current.push_back(ch);
                    }
                }
                else
                {
                    // 转义后的字符无论是什么都属于当前字段
                    current.push_back(ch);
                    esc = false;
                }
            }
            parts.push_back(current);
            return parts;
        }

#ifdef _WIN32
        // 将UTF-8转换为宽字符，便于Windows原生API显示中文
        std::wstring Utf8ToWide(const std::string& utf8)
        {
            if (utf8.empty()) return std::wstring();
            int wideLength = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
            if (wideLength <= 0) return std::wstring();
            std::wstring wide(static_cast<size_t>(wideLength), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wide[0], wideLength);
            if (!wide.empty() && wide.back() == L'\0') wide.pop_back();
            return wide;
        }

        // 读取系统是否为暗色主题
        bool IsSystemDarkMode()
        {
            HKEY key;
            DWORD value = 1; // 默认浅色
            DWORD size = sizeof(DWORD);
            if (RegOpenKeyExW(HKEY_CURRENT_USER,
                              L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                              0, KEY_READ, &key) == ERROR_SUCCESS)
            {
                RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, nullptr, reinterpret_cast<LPBYTE>(&value), &size);
                RegCloseKey(key);
            }
            return value == 0; // 0 表示暗色
        }

        // 为窗口应用 DWM 视觉效果（暗色、圆角）
        void ApplyDwmWindowAttributes(HWND hwnd, bool useDark)
        {
            // 暗色标题栏
            BOOL dark = useDark ? TRUE : FALSE;
            const DWORD DWMWA_USE_IMMERSIVE_DARK_MODE = 20u; // 在较新SDK里有定义，这里直接使用常量
            DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

            // 圆角
            const DWORD DWMWA_WINDOW_CORNER_PREFERENCE = 33u;
            enum DwmWindowCornerPreference { DWMWCP_DEFAULT = 0, DWMWCP_DONOTROUND = 1, DWMWCP_ROUND = 2, DWMWCP_ROUNDSMALL = 3 };
            DwmWindowCornerPreference pref = DWMWCP_ROUND;
            DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
        }

        float GetDpiScale(HWND hwnd)
        {
            // Prefer GetDpiForWindow on Win10+
            HMODULE user32 = LoadLibraryW(L"user32.dll");
            if (user32)
            {
                typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
                auto pGetDpiForWindow = (GetDpiForWindowFn)GetProcAddress(user32, "GetDpiForWindow");
                if (pGetDpiForWindow)
                {
                    UINT dpi = pGetDpiForWindow(hwnd);
                    FreeLibrary(user32);
                    return dpi > 0 ? (float)dpi / 96.0f : 1.0f;
                }
                FreeLibrary(user32);
            }
            // Fallback: assume 1.0
            return 1.0f;
        }
#endif

        // 只读可选择文本（单行），外观尽量接近普通文本
        void DrawCopyableText(const char* id, const std::string& text)
        {
            std::vector<char> buf(text.begin(), text.end());
            buf.push_back('\0');
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
            ImGui::InputText(id, buf.data(), (size_t)buf.size(), ImGuiInputTextFlags_ReadOnly);
            ImGui::PopStyleColor();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            ImGui::PopStyleVar();
        }

        // 只读可选择文本（多行自动换行），外观尽量接近普通文本
        void DrawCopyableMultiline(const char* id, const std::string& text)
        {
            std::vector<char> buf(text.begin(), text.end());
            buf.push_back('\0');
            // 计算与 TextWrapped 近似的高度
            float wrapWidth = ImGui::GetContentRegionAvail().x;
            if (wrapWidth <= 0.0f) wrapWidth = 400.0f;
            ImVec2 measured = ImGui::CalcTextSize(text.c_str(), nullptr, true, wrapWidth);
            float lineH = ImGui::GetTextLineHeightWithSpacing();
            float minH = lineH * 1.4f;
            float maxH = lineH * 6.0f;
            float height = measured.y + ImGui::GetStyle().FramePadding.y * 2.0f;
            height = (std::max)(minH, (std::min)(maxH, height));

            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
            ImGui::InputTextMultiline(id, buf.data(), (size_t)buf.size(), ImVec2(-1, height),
                                      ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_NoHorizontalScroll);
            ImGui::PopStyleColor();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            ImGui::PopStyleVar();
        }
    }
}
