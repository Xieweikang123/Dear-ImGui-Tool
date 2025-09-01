#include "word_reminder.h"
#include "imgui.h"
#include "replace_tool.h"
#include <string>
#include <vector>
#include <memory>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellscalingapi.h>
#include <dwmapi.h>
#include <commdlg.h>
#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "Shcore.lib")
#pragma comment(lib, "Comdlg32.lib")
#endif
#include <algorithm>

namespace WordReminder
{
    using ReplaceTool::AppendLog;
    // 内部状态管理
    struct FeatureState
    {
        bool enabled = true;
        bool windowOpen = true;
        std::vector<WordEntry> words;
        
        // UI状态
        char newWord[256] = "";
        char newMeaning[512] = "";
        char newPronunciation[256] = "";
        int reminderSeconds = 5; // 默认5秒（用于测试）
        int reminderType = 0; // 0: 自定义, 1: 快速预设
        bool showReminderPopup = false;
        int selectedWordIndex = -1;
        // 编辑暂存
        bool isEditing = false;
        char editWord[256] = "";
        char editMeaning[512] = "";
        
        // 设置
        bool autoShowReminders = true;
        bool playSoundOnReminder = false;
        int defaultReminderSeconds = 5; // 默认5秒（用于测试）
        
        // 统计
        int totalWords = 0;
        int reviewedToday = 0;
        int dueWords = 0;
        
        FeatureState() {}
    };
    
    static std::unique_ptr<FeatureState> g_state;
    
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
    
    // 只读可选择文本（单行），外观尽量接近普通文本
    static void DrawCopyableText(const char* id, const std::string& text)
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
    static void DrawCopyableMultiline(const char* id, const std::string& text)
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
        height = std::max(minH, std::min(maxH, height));

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

    // 对字段进行转义与反转义，避免分隔符与换行破坏一行一条记录的约定
    static std::string EscapeField(const std::string& input)
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

    static std::string UnescapeField(const std::string& input)
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
    static std::vector<std::string> SplitByUnescapedPipe(const std::string& line)
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

    // 保存单词到文件 (UTF-8)
    void SaveWords()
    {
        std::ofstream file("word_reminder_data.txt", std::ios::binary);
        if (file.is_open())
        {
            // 写入 BOM 以便在一些编辑器中正确显示
            const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
            file.write(reinterpret_cast<const char*>(bom), 3);
            for (const auto& entry : g_state->words)
            {
                file << EscapeField(entry.word) << "|"
                     << EscapeField(entry.meaning) << "|"
                     << EscapeField(entry.pronunciation) << "|"
                     << std::chrono::system_clock::to_time_t(entry.remindTime) << "|"
                     << entry.isActive << "|"
                     << entry.isMastered << "|"
                     << entry.reviewCount << "|"
                     << std::chrono::system_clock::to_time_t(entry.lastReview) << "\n";
            }
            file.close();
        }
    }
    
    // 从文件加载单词 (UTF-8)
    void LoadWords()
    {
        std::ifstream file("word_reminder_data.txt", std::ios::binary);
        if (file.is_open())
        {
            std::string line;
            // 跳过 UTF-8 BOM（若存在）
            char bom[3] = {0};
            file.read(bom, 3);
            if (!(static_cast<unsigned char>(bom[0]) == 0xEF && static_cast<unsigned char>(bom[1]) == 0xBB && static_cast<unsigned char>(bom[2]) == 0xBF))
            {
                // 无BOM，则回退到文件开头
                file.clear();
                file.seekg(0, std::ios::beg);
            }
            std::string buffer;
            while (std::getline(file, line))
            {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!buffer.empty()) buffer += "\n"; // 为兼容旧数据，将换行保留到缓冲区
                buffer += line;

                // 按未被转义的分隔符分割
                auto parts = SplitByUnescapedPipe(buffer);
                // 旧数据至少包含：word | meaning | pronunciation | remindTime | isActive
                if (parts.size() < 5)
                {
                    // 字段不足，继续读取下一行（说明有未转义的换行打断了记录）
                    continue;
                }

                // 解析一个完整记录
                WordEntry entry;
                entry.word = UnescapeField(parts[0]);
                entry.meaning = UnescapeField(parts[1]);
                entry.pronunciation = UnescapeField(parts[2]);

                auto to_time = [](const std::string& s) -> time_t {
                    try { return (time_t)std::stoll(s); } catch (...) { return (time_t)std::time(nullptr); }
                };
                auto to_bool = [](const std::string& s) -> bool {
                    return (s == "1" || s == "true" || s == "True" || s == "TRUE");
                };
                auto to_int = [](const std::string& s) -> int {
                    try { return (int)std::stol(s); } catch (...) { return 0; }
                };

                entry.remindTime = std::chrono::system_clock::from_time_t(to_time(parts[3]));
                entry.isActive = to_bool(parts[4]);

                bool hasMastered = parts.size() >= 6;
                bool hasReview = parts.size() >= 7;
                bool hasLastReview = parts.size() >= 8;

                entry.isMastered = hasMastered ? to_bool(parts[5]) : false;
                entry.reviewCount = hasReview ? to_int(parts[6]) : 0;
                entry.lastReview = std::chrono::system_clock::from_time_t(
                    hasLastReview ? to_time(parts[7]) : to_time(parts[3])
                );

                g_state->words.push_back(entry);
                buffer.clear();
            }
            file.close();
        }
    }
    
    void Initialize()
    {
        if (!g_state)
        {
            g_state = std::make_unique<FeatureState>();
        }
        
        LoadWords();
        
        // 更新统计信息
        g_state->totalWords = static_cast<int>(g_state->words.size());
        
        auto now = std::chrono::system_clock::now();
        g_state->dueWords = 0;
        g_state->reviewedToday = 0;
        
        auto today = std::chrono::system_clock::to_time_t(now);
        std::tm* tm = std::localtime(&today);
        auto todayStart = std::chrono::system_clock::from_time_t(
            std::mktime(new std::tm{0, 0, 0, tm->tm_mday, tm->tm_mon, tm->tm_year})
        );
        
        for (const auto& entry : g_state->words)
        {
            if (entry.isActive && !entry.isMastered && entry.remindTime <= now)
            {
                g_state->dueWords++;
            }
            
            if (entry.lastReview >= todayStart)
            {
                g_state->reviewedToday++;
            }
        }
    }

    // 重新计算统计信息
    static void RecomputeStats()
    {
        if (!g_state) return;

        g_state->totalWords = static_cast<int>(g_state->words.size());

        auto now = std::chrono::system_clock::now();
        g_state->dueWords = 0;
        g_state->reviewedToday = 0;

        auto today_t = std::chrono::system_clock::to_time_t(now);
        std::tm local_tm = *std::localtime(&today_t);
        std::tm start_tm = local_tm;
        start_tm.tm_hour = 0; start_tm.tm_min = 0; start_tm.tm_sec = 0;
        auto todayStart = std::chrono::system_clock::from_time_t(std::mktime(&start_tm));

        for (const auto& entry : g_state->words)
        {
            if (entry.isActive && !entry.isMastered && entry.remindTime <= now)
            {
                g_state->dueWords++;
            }
            if (entry.lastReview >= todayStart)
            {
                g_state->reviewedToday++;
            }
        }
    }
    
    void Cleanup()
    {
        if (g_state)
        {
            SaveWords();
            g_state.reset();
        }
    }

#ifdef _WIN32
    // 将UTF-8转换为宽字符，便于Windows原生API显示中文
    static std::wstring Utf8ToWide(const std::string& utf8)
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
    static bool IsSystemDarkMode()
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
    static void ApplyDwmWindowAttributes(HWND hwnd, bool useDark)
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

    // 右上角自定义置顶弹窗
    static HWND g_reminderHwnd = nullptr;
    static std::wstring g_reminderText;
    static std::vector<std::wstring> g_wordList;  // 存储单词列表
    static std::vector<std::wstring> g_meaningList;  // 存储释义列表
    static HFONT g_fontTitle = nullptr;
    static HFONT g_fontText = nullptr;
    static HFONT g_fontWord = nullptr;  // 专门用于单词的字体
    static HFONT g_fontButton = nullptr;
    static bool g_darkMode = false;
    static BYTE g_animOpacity = 0; // 0-255，用于淡入
    static HBRUSH g_btnBgBrush = nullptr; // 为按钮提供与父窗口一致的背景
    static int g_scrollPos = 0;
    static int g_scrollMax = 0;
    static HBRUSH g_cardBrush = nullptr; // 内容卡片背景，用于编辑框背景
    static POINT g_windowPosition = {-1, -1}; // 记住窗口位置，-1表示使用默认位置
    static HWND g_textEdit = nullptr; // 可复制阅读的只读文本控件
    
    // 状态管理变量 - 用于跟踪当前显示的单词列表
    static std::vector<WordEntry> g_currentDisplayedWords; // 当前正在窗口上显示的单词列表
    static bool g_windowShouldBeVisible = false; // 窗口是否应该可见

    enum ReminderCmdIds { BTN_REVIEWED = 1001, BTN_SNOOZE = 1002, BTN_CLOSE = 1003, BTN_COPY = 1004 };

#ifdef _WIN32
    static float GetDpiScale(HWND hwnd)
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

    static float GetSystemDpiScale()
    {
        // Try Win10+ system DPI
        HMODULE user32 = LoadLibraryW(L"user32.dll");
        if (user32)
        {
            typedef UINT (WINAPI *GetDpiForSystemFn)();
            auto pGetDpiForSystem = (GetDpiForSystemFn)GetProcAddress(user32, "GetDpiForSystem");
            if (pGetDpiForSystem)
            {
                UINT dpi = pGetDpiForSystem();
                FreeLibrary(user32);
                return dpi > 0 ? (float)dpi / 96.0f : 1.0f;
            }
            FreeLibrary(user32);
        }
        // Fallback: primary monitor DPI
        HMONITOR hMon = MonitorFromPoint(POINT{0,0}, MONITOR_DEFAULTTOPRIMARY);
        UINT dx = 96, dy = 96;
        GetDpiForMonitor(hMon, MDT_EFFECTIVE_DPI, &dx, &dy);
        return (float)dx / 96.0f;
    }
#endif

    // 文件对话框与导入导出辅助函数（Windows）
#ifdef _WIN32
    static bool ShowSaveFileDialog(std::wstring& outPath)
    {
        wchar_t fileBuffer[MAX_PATH] = L"";
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = nullptr;
        static const wchar_t* filter = L"文本文件 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0\0";
        ofn.lpstrFilter = filter;
        ofn.lpstrFile = fileBuffer;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        ofn.lpstrDefExt = L"txt";
        if (GetSaveFileNameW(&ofn))
        {
            outPath = fileBuffer;
            return true;
        }
        return false;
    }

    static bool ShowOpenFileDialog(std::wstring& outPath)
    {
        wchar_t fileBuffer[MAX_PATH] = L"";
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = nullptr;
        static const wchar_t* filter = L"文本文件 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0\0";
        ofn.lpstrFilter = filter;
        ofn.lpstrFile = fileBuffer;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&ofn))
        {
            outPath = fileBuffer;
            return true;
        }
        return false;
    }

    static void ExportWordsToPath(const std::wstring& savePath)
    {
        // 先保存到默认数据文件，然后复制
        SaveWords();
        CopyFileW(L"word_reminder_data.txt", savePath.c_str(), FALSE);
    }

    static bool ImportWordsFromPath(const std::wstring& openPath)
    {
        // 将选择的文件复制为默认数据文件，然后重新加载状态
        if (!CopyFileW(openPath.c_str(), L"word_reminder_data.txt", FALSE))
        {
            return false;
        }
        g_state->words.clear();
        LoadWords();
        RecomputeStats();
        return true;
    }
#endif

    static void MarkAllDueReviewed()
    {
        for (int i = 0; i < static_cast<int>(g_state->words.size()); i++)
        {
            auto& entry = g_state->words[i];
            if (entry.isActive && entry.remindTime <= std::chrono::system_clock::now())
            {
                MarkAsReviewed(i);
            }
        }
    }

    static void SnoozeAllDueFiveMinutes()
    {
        for (auto& entry : g_state->words)
        {
            if (entry.isActive && entry.remindTime <= std::chrono::system_clock::now())
            {
                entry.remindTime = std::chrono::system_clock::now() + std::chrono::minutes(5);
            }
        }
        SaveWords();
    }

    static int MeasureTextWidth(HFONT font, const wchar_t* text)
    {
        HDC hdc = GetDC(nullptr);
        HFONT old = nullptr;
        if (font) old = (HFONT)SelectObject(hdc, font);
        SIZE sz{0,0};
        GetTextExtentPoint32W(hdc, text, (int)wcslen(text), &sz);
        if (old) SelectObject(hdc, old);
        ReleaseDC(nullptr, hdc);
        return sz.cx;
    }

    static int IdealButtonWidth(const wchar_t* label)
    {
        int textW = MeasureTextWidth(g_fontButton, label);
        return textW + 36; // padding for left/right and round-rect margins
    }

    static void LayoutButtons(HWND hwnd)
    {
        RECT rc; GetClientRect(hwnd, &rc);
        int w1 = std::max<int>(110, IdealButtonWidth(L"标记已复习"));
        int w2 = std::max<int>(110, IdealButtonWidth(L"稍后提醒"));
        int w3 = std::max<int>(80, IdealButtonWidth(L"复制"));
        int w4 = std::max<int>(80, IdealButtonWidth(L"关闭"));
        int btnHeight = 50, gap = 8;
        int totalWidth = w1 + w2 + w3 + w4 + gap * 3;
        int startX = rc.right - totalWidth - 14;
        int y = rc.bottom - btnHeight - 12;
        HWND btnReviewed = GetDlgItem(hwnd, BTN_REVIEWED);
        HWND btnSnooze = GetDlgItem(hwnd, BTN_SNOOZE);
        HWND btnCopy = GetDlgItem(hwnd, BTN_COPY);
        HWND btnClose = GetDlgItem(hwnd, BTN_CLOSE);
        if (btnReviewed) MoveWindow(btnReviewed, startX, y, w1, btnHeight, TRUE);
        if (btnSnooze)   MoveWindow(btnSnooze,   startX + w1 + gap, y, w2, btnHeight, TRUE);
        if (btnCopy)     MoveWindow(btnCopy,     startX + w1 + gap + w2 + gap, y, w3, btnHeight, TRUE);
        if (btnClose)    MoveWindow(btnClose,    startX + w1 + gap + w2 + gap + w3 + gap, y, w4, btnHeight, TRUE);
    }

    static LRESULT CALLBACK ReminderWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
            case WM_CREATE:
            {
                // Fonts
                const float s = GetDpiScale(hwnd);
                if (!g_fontTitle)
                {
                    g_fontTitle = CreateFontW((int)(20 * s), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                }
                if (!g_fontText)
                {
                    g_fontText = CreateFontW((int)(16 * s), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                }
                // 创建专门用于单词的字体（更大更粗）
                if (!g_fontWord)
                {
                    g_fontWord = CreateFontW((int)(20 * s), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                }
                if (!g_fontButton)
                {
                    g_fontButton = CreateFontW((int)(15 * s), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                }

                HWND b1 = CreateWindowW(L"BUTTON", L"标记已复习",
                              WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                              0, 0, 0, 0, hwnd, (HMENU)BTN_REVIEWED, GetModuleHandleW(nullptr), nullptr);
                HWND b2 = CreateWindowW(L"BUTTON", L"稍后提醒",
                              WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                              0, 0, 0, 0, hwnd, (HMENU)BTN_SNOOZE, GetModuleHandleW(nullptr), nullptr);
                HWND b3 = CreateWindowW(L"BUTTON", L"复制",
                              WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                              0, 0, 0, 0, hwnd, (HMENU)BTN_COPY, GetModuleHandleW(nullptr), nullptr);
                HWND b4 = CreateWindowW(L"BUTTON", L"关闭",
                              WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                              0, 0, 0, 0, hwnd, (HMENU)BTN_CLOSE, GetModuleHandleW(nullptr), nullptr);
                if (b1) SendMessageW(b1, WM_SETFONT, (WPARAM)g_fontButton, TRUE);
                if (b2) SendMessageW(b2, WM_SETFONT, (WPARAM)g_fontButton, TRUE);
                if (b3) SendMessageW(b3, WM_SETFONT, (WPARAM)g_fontButton, TRUE);
                if (b4) SendMessageW(b4, WM_SETFONT, (WPARAM)g_fontButton, TRUE);
                LayoutButtons(hwnd);
                SetTimer(hwnd, 1, 15000, nullptr); // 15秒自动关闭

                // 应用系统主题及DWM效果
                g_darkMode = IsSystemDarkMode();
                ApplyDwmWindowAttributes(hwnd, g_darkMode);

                // 不再使用编辑框，直接绘制文本

                // 预备卡片背景画刷，供编辑框背景使用
                if (g_cardBrush) { DeleteObject(g_cardBrush); g_cardBrush = nullptr; }
                {
                    COLORREF clrCard = g_darkMode ? RGB(43, 43, 48) : RGB(255, 255, 255);
                    g_cardBrush = CreateSolidBrush(clrCard);
                }

                // 准备按钮背景刷（与父窗口背景一致，避免四角发白）
                {
                    COLORREF clrWnd = g_darkMode ? RGB(32, 32, 36) : RGB(245, 246, 248);
                    if (g_btnBgBrush) { DeleteObject(g_btnBgBrush); g_btnBgBrush = nullptr; }
                    g_btnBgBrush = CreateSolidBrush(clrWnd);
                }

                // 初始透明并淡入
                g_animOpacity = 0;
                SetLayeredWindowAttributes(hwnd, 0, g_animOpacity, LWA_ALPHA);
                SetTimer(hwnd, 2, 15, nullptr); // 每15ms 提升透明度
                return 0;
            }
            case WM_VSCROLL:
            {
                int scrollCode = LOWORD(wParam);
                int pos = HIWORD(wParam);
                
                // 添加滚动调试日志
                AppendLog("[滚动调试] WM_VSCROLL: code=" + std::to_string(scrollCode) + 
                         ", pos=" + std::to_string(pos) + 
                         ", 当前scrollPos=" + std::to_string(g_scrollPos) + 
                         ", scrollMax=" + std::to_string(g_scrollMax));
                
                switch (scrollCode)
                {
                    case SB_LINEUP:
                        g_scrollPos = std::max(0, g_scrollPos - 20);
                        AppendLog("[滚动调试] SB_LINEUP: 新scrollPos=" + std::to_string(g_scrollPos));
                        break;
                    case SB_LINEDOWN:
                        g_scrollPos = std::min(g_scrollMax, g_scrollPos + 20);
                        AppendLog("[滚动调试] SB_LINEDOWN: 新scrollPos=" + std::to_string(g_scrollPos));
                        break;
                    case SB_PAGEUP:
                        g_scrollPos = std::max(0, g_scrollPos - 100);
                        AppendLog("[滚动调试] SB_PAGEUP: 新scrollPos=" + std::to_string(g_scrollPos));
                        break;
                    case SB_PAGEDOWN:
                        g_scrollPos = std::min(g_scrollMax, g_scrollPos + 100);
                        AppendLog("[滚动调试] SB_PAGEDOWN: 新scrollPos=" + std::to_string(g_scrollPos));
                        break;
                    case SB_THUMBTRACK:
                    case SB_THUMBPOSITION:
                        g_scrollPos = pos;
                        AppendLog("[滚动调试] SB_THUMB: 类型=" + std::string(scrollCode == SB_THUMBTRACK ? "TRACK" : "POSITION") + 
                                 ", pos=" + std::to_string(pos) + ", 新scrollPos=" + std::to_string(g_scrollPos));
                        break;
                }
                
                SetScrollPos(hwnd, SB_VERT, g_scrollPos, TRUE);
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }
            case WM_MOUSEWHEEL:
            {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                int oldScrollPos = g_scrollPos;
                g_scrollPos = std::max(0, std::min(g_scrollMax, g_scrollPos - delta / 4));
                
                // 添加鼠标滚轮调试日志
                AppendLog("[滚动调试] WM_MOUSEWHEEL: delta=" + std::to_string(delta) + 
                         ", 旧scrollPos=" + std::to_string(oldScrollPos) + 
                         ", 新scrollPos=" + std::to_string(g_scrollPos) + 
                         ", scrollMax=" + std::to_string(g_scrollMax));
                
                SetScrollPos(hwnd, SB_VERT, g_scrollPos, TRUE);
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }
            case WM_SIZE:
            {
                LayoutButtons(hwnd);
                return 0;
            }
            case WM_MOVE:
            {
                // 记住用户拖拽后的窗口位置
                RECT rc;
                GetWindowRect(hwnd, &rc);
                g_windowPosition.x = rc.left;
                g_windowPosition.y = rc.top;
                return 0;
            }
            case WM_COMMAND:
            {
                int id = LOWORD(wParam);
                if (id == BTN_REVIEWED)
                {
                    MarkAllDueReviewed();
                    g_state->showReminderPopup = false;
                    ShowWindow(hwnd, SW_HIDE);
                    g_windowShouldBeVisible = false;
                    
                    // 检查是否还有其他需要复习的单词，如果有则立即显示新窗口
                    if (HasReminderToShow())
                    {
                        g_state->showReminderPopup = true;
                    }
                    return 0;
                }
                if (id == BTN_SNOOZE)
                {
                    SnoozeAllDueFiveMinutes();
                    g_state->showReminderPopup = false;
                    ShowWindow(hwnd, SW_HIDE);
                    g_windowShouldBeVisible = false;
                    
                    // 检查是否还有其他需要复习的单词，如果有则立即显示新窗口
                    if (HasReminderToShow())
                    {
                        g_state->showReminderPopup = true;
                    }
                    return 0;
                }
                if (id == BTN_COPY)
                {
                    // 只复制单词到剪贴板
                    std::wstring wordsOnly;
                    for (const auto& entry : g_currentDisplayedWords)
                    {
                        if (!wordsOnly.empty())
                        {
                            wordsOnly += L"\n";
                        }
                        wordsOnly += Utf8ToWide(entry.word);
                    }
                    
                    if (!wordsOnly.empty())
                    {
                        // 转换为UTF-8格式
                        std::string utf8Text;
                        int len = WideCharToMultiByte(CP_UTF8, 0, wordsOnly.c_str(), -1, nullptr, 0, nullptr, nullptr);
                        if (len > 0)
                        {
                            utf8Text.resize(len - 1);
                            WideCharToMultiByte(CP_UTF8, 0, wordsOnly.c_str(), -1, &utf8Text[0], len, nullptr, nullptr);
                        }
                        
                        // 打开剪贴板
                        if (OpenClipboard(hwnd))
                        {
                            EmptyClipboard();
                            
                            // 分配全局内存
                            HGLOBAL hClipboardData = GlobalAlloc(GMEM_DDESHARE, utf8Text.length() + 1);
                            if (hClipboardData)
                            {
                                char* pchData = (char*)GlobalLock(hClipboardData);
                                if (pchData)
                                {
                                    strcpy_s(pchData, utf8Text.length() + 1, utf8Text.c_str());
                                    GlobalUnlock(hClipboardData);
                                    SetClipboardData(CF_TEXT, hClipboardData);
                                }
                            }
                            CloseClipboard();
                        }
                    }
                    return 0;
                }
                if (id == BTN_CLOSE)
                {
                    g_state->showReminderPopup = false;
                    ShowWindow(hwnd, SW_HIDE);
                    g_windowShouldBeVisible = false;
                    return 0;
                }
                break;
            }
            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd, &ps);
                RECT rc;
                GetClientRect(hwnd, &rc);
                
                // 双缓冲实现 - 创建内存DC和位图
                HDC memDC = CreateCompatibleDC(hdc);
                HBITMAP memBitmap = CreateCompatibleBitmap(hdc, rc.right - rc.left, rc.bottom - rc.top);
                HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);
                
                // 背景 - 绘制到内存DC
                COLORREF clrWnd = g_darkMode ? RGB(32, 32, 36) : RGB(245, 246, 248);
                HBRUSH bgWnd = CreateSolidBrush(clrWnd);
                FillRect(memDC, &rc, bgWnd);
                DeleteObject(bgWnd);

                // 内容卡片 - 绘制到内存DC
                RECT content = { rc.left + 14, rc.top + 14, rc.right - 14, rc.bottom - 58 };
                COLORREF clrCard = g_darkMode ? RGB(43, 43, 48) : RGB(255, 255, 255);
                COLORREF clrBorder = g_darkMode ? RGB(64, 64, 72) : RGB(222, 226, 232);
                HBRUSH brCard = CreateSolidBrush(clrCard);
                HPEN pnCard = CreatePen(PS_SOLID, 1, clrBorder);
                HGDIOBJ oldPen = SelectObject(memDC, pnCard);
                HGDIOBJ oldBrush = SelectObject(memDC, brCard);
                RoundRect(memDC, content.left, content.top, content.right, content.bottom, 10, 10);
                SelectObject(memDC, oldBrush);
                SelectObject(memDC, oldPen);
                DeleteObject(brCard);
                DeleteObject(pnCard);

                // 左侧色条强调 - 绘制到内存DC
                HBRUSH brAccent = CreateSolidBrush(RGB(45, 140, 255));
                RECT accent = { content.left, content.top, content.left + 3, content.bottom };
                FillRect(memDC, &accent, brAccent);
                DeleteObject(brAccent);

                // 标题 - 绘制到内存DC
                SetBkMode(memDC, TRANSPARENT);
                SetTextColor(memDC, g_darkMode ? RGB(240, 240, 240) : RGB(28, 28, 30));
                if (g_fontTitle) SelectObject(memDC, g_fontTitle);
                RECT titleRc = { content.left + 10, content.top + 8, content.right - 10, content.top + 36 };
                
                // 标题文本 - 移除标题显示
                // std::wstring titleText = L"📚 单词复习提醒";
                // DrawTextW(memDC, titleText.c_str(), -1, &titleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                // 直接绘制文本内容，单词使用粗体字体，支持滚动
                int yOffset = content.top + 40 - g_scrollPos;
                AppendLog("[滚动调试] 文本绘制: yOffset=" + std::to_string(yOffset) + 
                         ", content.top=" + std::to_string(content.top) + 
                         ", g_scrollPos=" + std::to_string(g_scrollPos) + 
                         ", 文本长度=" + std::to_string(g_reminderText.length()));
                if (!g_reminderText.empty())
                {
                    // 解析文本，分别绘制单词（粗体）和释义（普通）
                    std::wstring text = g_reminderText;
                    size_t pos = 0;
                    int currentY = yOffset;
                    
                    while (pos < text.length())
                    {
                        size_t lineEnd = text.find(L'\n', pos);
                        if (lineEnd == std::wstring::npos) lineEnd = text.length();
                        
                        std::wstring line = text.substr(pos, lineEnd - pos);
                        
                        // 检查是否是单词行（包含📖图标）
                        if (line.find(L"📖") != std::wstring::npos)
                        {
                            // 单词行：使用粗体字体
                            if (g_fontWord) SelectObject(memDC, g_fontWord);
                            SetTextColor(memDC, g_darkMode ? RGB(255, 255, 255) : RGB(0, 0, 0));
                        }
                        else
                        {
                            // 释义行：使用普通字体
                            if (g_fontText) SelectObject(memDC, g_fontText);
                            SetTextColor(memDC, g_darkMode ? RGB(220, 220, 225) : RGB(60, 60, 68));
                        }
                        
                        // 不限制宽度，让文本自然显示
                        RECT lineRc = { content.left + 10, currentY, rc.right - 10, content.bottom - 10 };
                        int textHeight = DrawTextW(memDC, line.c_str(), -1, &lineRc, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_CALCRECT);
                        
                        // 重新绘制，使用计算出的高度
                        lineRc.bottom = lineRc.top + textHeight;
                        DrawTextW(memDC, line.c_str(), -1, &lineRc, DT_LEFT | DT_TOP | DT_WORDBREAK);
                        
                        // 移动到下一行位置
                        currentY += textHeight + 8;
                        
                        pos = lineEnd + 1;
                    }
                }

                // 一次性将内存DC的内容复制到屏幕DC
                BitBlt(hdc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, memDC, 0, 0, SRCCOPY);
                
                // 清理GDI对象
                SelectObject(memDC, oldBitmap);
                DeleteObject(memBitmap);
                DeleteDC(memDC);

                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_DRAWITEM:
            {
                LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lParam;
                if (!dis) break;
                bool pressed = (dis->itemState & ODS_SELECTED) != 0;

                // 主要按钮（标记已复习）使用实心蓝色，复制按钮使用绿色，其余为浅色填充+边框
                int ctrlId = GetDlgCtrlID(dis->hwndItem);
                bool isPrimary = (ctrlId == BTN_REVIEWED);
                bool isCopy = (ctrlId == BTN_COPY);
                COLORREF primary = RGB(45, 140, 255);
                COLORREF primaryPressed = RGB(29, 112, 214);
                COLORREF copyColor = RGB(34, 197, 94);
                COLORREF copyPressed = RGB(22, 163, 74);
                COLORREF fill = g_darkMode ? RGB(58, 58, 64) : RGB(245, 247, 250);
                COLORREF border = g_darkMode ? RGB(80, 80, 88) : RGB(220, 224, 228);
                if (isPrimary) fill = pressed ? primaryPressed : primary;
                else if (isCopy) fill = pressed ? copyPressed : copyColor;

                HBRUSH b = CreateSolidBrush(fill);
                HPEN p = CreatePen(PS_SOLID, 1, (isPrimary || isCopy) ? RGB(30, 118, 224) : border);
                HGDIOBJ oldB = SelectObject(dis->hDC, b);
                HGDIOBJ oldP = SelectObject(dis->hDC, p);
                RoundRect(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom, 8, 8);
                SelectObject(dis->hDC, oldB);
                SelectObject(dis->hDC, oldP);
                DeleteObject(b);
                DeleteObject(p);
                SetBkMode(dis->hDC, TRANSPARENT);
                COLORREF txt = (isPrimary || isCopy) ? RGB(255,255,255) : (g_darkMode ? RGB(230,230,235) : RGB(40,40,44));
                SetTextColor(dis->hDC, txt);
                if (g_fontButton) SelectObject(dis->hDC, g_fontButton);
                wchar_t buf[128];
                GetWindowTextW(dis->hwndItem, buf, 128);
                DrawTextW(dis->hDC, buf, -1, (RECT*)&dis->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }
            case WM_SYSCOMMAND:
            {
                if ((wParam & 0xFFF0) == SC_CLOSE)
                {
                    // 屏蔽右上角关闭与 Alt+F4
                    return 0;
                }
                break;
            }
            case WM_CTLCOLORBTN:
            {
                HDC hdcBtn = (HDC)wParam;
                SetBkMode(hdcBtn, TRANSPARENT);
                // 返回与父窗口一致的背景刷，避免按钮圆角外漏出浅色
                COLORREF clrWnd = g_darkMode ? RGB(32, 32, 36) : RGB(245, 246, 248);
                LOGBRUSH lb{};
                if (g_btnBgBrush)
                {
                    GetObject(g_btnBgBrush, sizeof(lb), &lb);
                    COLORREF current = lb.lbColor;
                    if (current != clrWnd)
                    {
                        DeleteObject(g_btnBgBrush);
                        g_btnBgBrush = CreateSolidBrush(clrWnd);
                    }
                }
                else
                {
                    g_btnBgBrush = CreateSolidBrush(clrWnd);
                }
                return (LRESULT)g_btnBgBrush;
            }

                         case WM_KEYDOWN:
             {
                 if (wParam == VK_ESCAPE)
                 {
                     g_state->showReminderPopup = false;
                     ShowWindow(hwnd, SW_HIDE);
                     g_windowShouldBeVisible = false;
                     return 0;
                 }
                 break;
             }
                         case WM_TIMER:
             {
                 if (wParam == 1)
                 {
                     KillTimer(hwnd, 1);
                     g_state->showReminderPopup = false;
                     ShowWindow(hwnd, SW_HIDE);
                     g_windowShouldBeVisible = false;
                     
                     // 检查是否还有其他需要复习的单词，如果有则重新显示
                     if (HasReminderToShow())
                     {
                         g_state->showReminderPopup = true;
                     }
                     return 0;
                 }
                if (wParam == 2)
                {
                    if (g_animOpacity < 250)
                    {
                        g_animOpacity = (BYTE)std::min<int>(255, g_animOpacity + 25);
                        SetLayeredWindowAttributes(hwnd, 0, g_animOpacity, LWA_ALPHA);
                    }
                    else
                    {
                        g_animOpacity = 255;
                        SetLayeredWindowAttributes(hwnd, 0, g_animOpacity, LWA_ALPHA);
                        KillTimer(hwnd, 2);
                    }
                    return 0;
                }
                break;
            }
                         case WM_CLOSE:
             {
                 g_state->showReminderPopup = false;
                 ShowWindow(hwnd, SW_HIDE);
                 g_windowShouldBeVisible = false;
                 return 0;
             }
                         case WM_DESTROY:
             {
                 if (hwnd == g_reminderHwnd) 
                 {
                     g_reminderHwnd = nullptr;
                     g_windowShouldBeVisible = false;
                     g_currentDisplayedWords.clear();
                     
                     // 检查是否还有其他需要复习的单词
                     if (HasReminderToShow())
                     {
                         // 如果有，重新设置显示标志
                         g_state->showReminderPopup = true;
                     }
                     else
                     {
                         // 只有在没有需要复习的单词时才重置位置
                         g_windowPosition.x = -1;
                         g_windowPosition.y = -1;
                     }
                 }
                if (g_fontTitle) { DeleteObject(g_fontTitle); g_fontTitle = nullptr; }
                if (g_fontText) { DeleteObject(g_fontText); g_fontText = nullptr; }
        if (g_fontWord) { DeleteObject(g_fontWord); g_fontWord = nullptr; }
                if (g_fontButton) { DeleteObject(g_fontButton); g_fontButton = nullptr; }
                if (g_btnBgBrush) { DeleteObject(g_btnBgBrush); g_btnBgBrush = nullptr; }
                return 0;
            }
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    static void EnsureReminderWindow()
    {
        auto dueWords = GetDueWords();
        
        // 比较最新列表和当前显示的列表
        bool wordsChanged = false;
        if (dueWords.size() != g_currentDisplayedWords.size())
        {
            wordsChanged = true;
        }
        else
        {
            // 比较每个单词的内容
            for (size_t i = 0; i < dueWords.size(); ++i)
            {
                if (i >= g_currentDisplayedWords.size() || 
                    dueWords[i].word != g_currentDisplayedWords[i].word ||
                    dueWords[i].meaning != g_currentDisplayedWords[i].meaning)
                {
                    wordsChanged = true;
                    break;
                }
            }
        }
        
        // 更新当前显示的单词列表
        g_currentDisplayedWords = dueWords;
        
        if (dueWords.empty()) 
        { 
            // 如果没有需要复习的单词，隐藏窗口而不是销毁
            if (g_reminderHwnd) 
            {
                ShowWindow(g_reminderHwnd, SW_HIDE);
                g_windowShouldBeVisible = false;
            }
            g_state->showReminderPopup = false; 
            return; 
        }

        // 构建新的文本内容，使用特殊格式突出单词
        std::wstring fullText;
        for (size_t i = 0; i < dueWords.size(); ++i)
        {
            const auto& entry = dueWords[i];
            std::wstring word = Utf8ToWide(entry.word);
            std::wstring meaning = Utf8ToWide(entry.meaning);
            
            if (i > 0)
            {
                fullText += L"\n\n";
            }
            
            // 使用特殊符号和格式突出单词
            fullText += L"📖 " + word;
            fullText += L"\n    " + meaning;
        }
        
        // 如果窗口已经存在，检查内容是否需要更新
        if (g_reminderHwnd) 
        {
            // 只有当内容真正改变时才更新，避免频繁重绘
            if (g_reminderText != fullText)
            {
                g_reminderText = fullText;
                
                // 计算滚动条范围 - 只针对内容区域，不包含按钮
                HDC hdc = GetDC(g_reminderHwnd);
                HFONT old = nullptr;
                if (g_fontText) old = (HFONT)SelectObject(hdc, g_fontText);
                
                RECT rc;
                GetClientRect(g_reminderHwnd, &rc);
                RECT content = { rc.left + 14, rc.top + 14, rc.right - 14, rc.bottom - 58 };
                
                // 计算文本内容的总高度
                RECT measureRc = { 0, 0, content.right - content.left - 20, 2000 };
                int totalHeight = DrawTextW(hdc, g_reminderText.c_str(), -1, &measureRc, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_CALCRECT);
                
                // 内容区域的实际可用高度（减去上下边距）
                int contentAreaHeight = content.bottom - content.top - 80; // 减去上下边距
                g_scrollMax = std::max(0, totalHeight - contentAreaHeight);
                g_scrollPos = std::min(g_scrollPos, g_scrollMax);
                
                // 添加详细的滚动范围计算调试日志
                AppendLog("[滚动调试] 详细计算: measureRc宽度=" + std::to_string(measureRc.right - measureRc.left) + 
                         ", content区域=" + std::to_string(content.right - content.left) + "x" + std::to_string(content.bottom - content.top) + 
                         ", contentAreaHeight=" + std::to_string(contentAreaHeight) + 
                         ", totalHeight=" + std::to_string(totalHeight) + 
                         ", 文本内容='" + std::string(g_reminderText.begin(), g_reminderText.end()) + "'");
                
                // 添加滚动范围计算调试日志
                AppendLog("[滚动调试] 滚动范围计算: totalHeight=" + std::to_string(totalHeight) + 
                         ", contentAreaHeight=" + std::to_string(contentAreaHeight) + 
                         ", scrollMax=" + std::to_string(g_scrollMax) + 
                         ", scrollPos=" + std::to_string(g_scrollPos));
                
                // 测试：强制设置滚动范围，即使内容不超出
                if (g_scrollMax > 0 || !g_reminderText.empty())
                {
                    // 如果计算出的滚动范围太小，强制设置一个测试范围
                    if (g_scrollMax <= 0 && !g_reminderText.empty())
                    {
                        g_scrollMax = 200; // 强制设置200像素的滚动范围用于测试
                        AppendLog("[滚动调试] 强制设置滚动范围: " + std::to_string(g_scrollMax) + " (测试用)");
                    }
                    
                    SetScrollRange(g_reminderHwnd, SB_VERT, 0, g_scrollMax, TRUE);
                    SetScrollPos(g_reminderHwnd, SB_VERT, g_scrollPos, TRUE);
                    ShowScrollBar(g_reminderHwnd, SB_VERT, TRUE);
                    AppendLog("[滚动调试] 显示滚动条: 范围0-" + std::to_string(g_scrollMax) + 
                             ", 位置=" + std::to_string(g_scrollPos));
                }
                else
                {
                    ShowScrollBar(g_reminderHwnd, SB_VERT, FALSE);
                    g_scrollPos = 0;
                    AppendLog("[滚动调试] 隐藏滚动条: 内容不超出区域");
                }
                
                if (old) SelectObject(hdc, old);
                ReleaseDC(g_reminderHwnd, hdc);
                
                // 文本已直接绘制，无需同步到编辑框
                // 仍然请求重绘以更新背景与边框
                InvalidateRect(g_reminderHwnd, nullptr, FALSE);
            }
            
            // 确保窗口可见
            if (!g_windowShouldBeVisible)
            {
                ShowWindow(g_reminderHwnd, SW_SHOW);
                g_windowShouldBeVisible = true;
            }
            return;
        }

        // 设置文本内容
        g_reminderText = fullText;

        WNDCLASSW wc = {};
        wc.lpfnWndProc = ReminderWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"WordReminderPopupWindow";
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.style = CS_DROPSHADOW;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        static ATOM atom = RegisterClassW(&wc);
        (void)atom;

        // 使用固定尺寸，添加滚动条支持
        float s = GetSystemDpiScale();
        int width = (int)(500 * s);   // 固定合适的宽度
        int height = (int)(250 * s);  // 固定合适的高度

        // Ensure width fits all buttons
        int w1 = std::max<int>((int)(120 * s), IdealButtonWidth(L"标记已复习"));
        int w2 = std::max<int>((int)(120 * s), IdealButtonWidth(L"稍后提醒"));
        int w3 = std::max<int>((int)(120 * s), IdealButtonWidth(L"关闭"));
        int buttonsTotal = w1 + w2 + w3 + 16 * 2 + 32; // gaps + side margins
        width = std::max<int>(width, buttonsTotal);

        RECT workArea;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
        
        // 使用记住的位置，如果没有记住的位置则使用默认右上角位置
        int x, y;
        if (g_windowPosition.x >= 0 && g_windowPosition.y >= 0)
        {
            x = g_windowPosition.x;
            y = g_windowPosition.y;
            
            // 确保窗口不会超出工作区域
            if (x + width > workArea.right) x = workArea.right - width - 20;
            if (y + height > workArea.bottom) y = workArea.bottom - height - 20;
            if (x < workArea.left) x = workArea.left + 20;
            if (y < workArea.top) y = workArea.top + 20;
        }
        else
        {
            x = workArea.right - width - 20;
            y = workArea.top + 20;
        }

        g_reminderHwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_COMPOSITED,
            wc.lpszClassName,
            L"提醒",
            WS_CAPTION | WS_VSCROLL,
            x, y, width, height,
            nullptr, nullptr, wc.hInstance, nullptr);
        
        // 添加窗口创建调试日志
        AppendLog("[滚动调试] 窗口创建: 样式包含WS_VSCROLL, 尺寸=" + std::to_string(width) + "x" + std::to_string(height) + 
                 ", 位置=" + std::to_string(x) + "," + std::to_string(y) + 
                 ", 窗口句柄=" + (g_reminderHwnd ? "有效" : "无效"));

        if (g_reminderHwnd)
        {
            ShowWindow(g_reminderHwnd, SW_SHOWNORMAL);
            UpdateWindow(g_reminderHwnd);
            g_windowShouldBeVisible = true;
            // 文本已直接绘制，无需同步
        }
        else
        {
            // 回退：无法创建窗口则不再显示
            g_state->showReminderPopup = false;
            g_windowShouldBeVisible = false;
        }
    }
#endif
    
    const char* GetFeatureName()
    {
        return "单词学习提醒";
    }
    
    bool IsEnabled()
    {
        return g_state && g_state->enabled;
    }
    
    void SetEnabled(bool enabled)
    {
        if (g_state)
        {
            g_state->enabled = enabled;
        }
    }
    
    void AddWord(const std::string& word, const std::string& meaning, int secondsFromNow)
    {
        if (!g_state) return;
        
        WordEntry entry;
        entry.word = word;
        entry.meaning = meaning;
        entry.remindTime = std::chrono::system_clock::now() + std::chrono::seconds(secondsFromNow);
        entry.lastReview = std::chrono::system_clock::now();
        
        g_state->words.push_back(entry);
        g_state->totalWords++;
        SaveWords();
    }
    
    void RemoveWord(int index)
    {
        if (!g_state || index < 0 || index >= static_cast<int>(g_state->words.size())) return;
        
        g_state->words.erase(g_state->words.begin() + index);
        g_state->totalWords--;
        SaveWords();
    }
    
    void MarkAsReviewed(int index)
    {
        if (!g_state || index < 0 || index >= static_cast<int>(g_state->words.size())) return;
        
        auto& entry = g_state->words[index];
        entry.reviewCount++;
        entry.lastReview = std::chrono::system_clock::now();
        
        // 根据复习次数调整下次提醒时间
        int nextSeconds = 1800; // 30分钟
        if (entry.reviewCount == 1) nextSeconds = 3600; // 1小时
        else if (entry.reviewCount == 2) nextSeconds = 7200; // 2小时
        else if (entry.reviewCount == 3) nextSeconds = 14400; // 4小时
        else if (entry.reviewCount == 4) nextSeconds = 28800; // 8小时
        else if (entry.reviewCount >= 5) nextSeconds = 86400; // 24小时
        
        entry.remindTime = std::chrono::system_clock::now() + std::chrono::seconds(nextSeconds);
        
        SaveWords();
    }
    
    void MarkAsMastered(int index)
    {
        if (!g_state || index < 0 || index >= static_cast<int>(g_state->words.size())) return;
        
        auto& entry = g_state->words[index];
        entry.isMastered = true;
        entry.lastReview = std::chrono::system_clock::now();
        
        SaveWords();
    }
    
    void UnmarkAsMastered(int index)
    {
        if (!g_state || index < 0 || index >= static_cast<int>(g_state->words.size())) return;
        
        auto& entry = g_state->words[index];
        entry.isMastered = false;
        entry.lastReview = std::chrono::system_clock::now();
        
        // 重新设置提醒时间为5分钟后
        entry.remindTime = std::chrono::system_clock::now() + std::chrono::seconds(300);
        
        SaveWords();
    }
    
    int GetMasteredWordsCount()
    {
        if (!g_state) return 0;
        
        int count = 0;
        for (const auto& entry : g_state->words)
        {
            if (entry.isMastered)
            {
                count++;
            }
        }
        return count;
    }
    
    int GetTotalWordsCount()
    {
        if (!g_state) return 0;
        return static_cast<int>(g_state->words.size());
    }
    
    bool HasReminderToShow()
    {
        if (!g_state || !g_state->autoShowReminders) return false;
        
        // 检查是否有需要复习的单词
        auto now = std::chrono::system_clock::now();
        for (const auto& entry : g_state->words)
        {
            if (entry.isActive && !entry.isMastered && entry.remindTime <= now)
            {
                return true;
            }
        }
        return false;
    }
    
    std::vector<WordEntry> GetDueWords()
    {
        std::vector<WordEntry> result;
        if (!g_state) return result;
        
        auto now = std::chrono::system_clock::now();
        for (const auto& entry : g_state->words)
        {
            if (entry.isActive && !entry.isMastered && entry.remindTime <= now)
            {
                result.push_back(entry);
            }
        }
        return result;
    }
    
    void DrawUI()
    {
        if (!g_state || !g_state->enabled)
            return;
            
        // 创建主窗口
        ImGui::SetNextWindowSize(ImVec2(1280, 720), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("单词学习提醒##MainWindow", &g_state->windowOpen))
        {
            ImGui::End();
            return;
        }
        
        // 统计信息区域
        {
            const float uiScale = ImGui::GetFontSize() / 16.0f;
            ImGui::BeginChild("Stats", ImVec2(0, 60.0f * uiScale), false);
        }
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "📊 学习统计");
        ImGui::Separator();
        
        ImGui::Columns(5, "stats");
        ImGui::Text("总单词数: %d", GetTotalWordsCount());
        ImGui::NextColumn();
        ImGui::Text("已掌握: %d", GetMasteredWordsCount());
        ImGui::NextColumn();
        ImGui::Text("今日复习: %d", g_state->reviewedToday);
        ImGui::NextColumn();
        ImGui::Text("待复习: %d", g_state->dueWords);
        ImGui::NextColumn();
        ImGui::Text("学习中: %d", GetTotalWordsCount() - GetMasteredWordsCount());
        ImGui::Columns(1);
        ImGui::EndChild();
        
        // 添加新单词区域
        ImGui::Spacing();
        if (ImGui::CollapsingHeader("➕ 添加新单词", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const float uiScale = ImGui::GetFontSize() / 16.0f;
            ImGui::BeginChild("AddWord", ImVec2(0, 200.0f * uiScale), false);
            
            ImGui::Columns(2, "add_word");
            ImGui::SetColumnWidth(0, 150.0f * uiScale);
            
            ImGui::Text("单词:");
            ImGui::SameLine();
            ImGui::InputText("##Word", g_state->newWord, sizeof(g_state->newWord));
            
            // 已移除音标输入
            ImGui::NextColumn();
            
            ImGui::NextColumn();
            ImGui::Text("释义:");
            ImGui::SameLine();
            ImGui::InputTextMultiline("##Meaning", g_state->newMeaning, sizeof(g_state->newMeaning), ImVec2(-1, 80.0f * uiScale));
            
                         ImGui::NextColumn();
             
            // 提醒时间设置区域（预设 + 自定义分钟）
            ImGui::Text("提醒时间:");
            ImGui::SameLine();
            
            // 预设按钮（按 DPI/字号缩放）
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f * uiScale, 6.0f * uiScale));
            const char* presets[] = {"5秒","30秒","1分钟","5分钟","10分钟","15分钟","30分钟","1小时","2小时","4小时"};
            int presetSeconds[] = {5,30,60,300,600,900,1800,3600,7200,14400};
            for (int i = 0; i < 10; ++i)
            {
                if (i > 0) ImGui::SameLine();
                bool isSelected = (g_state->reminderSeconds == presetSeconds[i]);
                if (isSelected)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 1.0f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.7f, 1.0f, 1.0f));
                }
                if (ImGui::Button(presets[i], ImVec2(72.0f * uiScale, 28.0f * uiScale)))
                {
                    g_state->reminderSeconds = presetSeconds[i];
                }
                if (isSelected)
                {
                    ImGui::PopStyleColor();
                    ImGui::PopStyleColor();
                }
            }
            ImGui::PopStyleVar();
            
            ImGui::Spacing();
            ImGui::Text("自定义时间(分钟):");
            static int minutesOnly = 30;
            if (ImGui::IsWindowAppearing())
            {
                minutesOnly = std::max(1, g_state->reminderSeconds / 60);
            }
            if (ImGui::SliderInt("##MinutesOnly", &minutesOnly, 1, 240, "%d 分钟"))
            {
                g_state->reminderSeconds = minutesOnly * 60;
            }
            ImGui::Columns(1);
            
            
            ImGui::Spacing();
            if (ImGui::Button("添加单词", ImVec2(-1, 0)))
            {
                if (strlen(g_state->newWord) > 0)
                {
                    AddWord(g_state->newWord, g_state->newMeaning, g_state->reminderSeconds);
                    
                    // 清空输入
                    memset(g_state->newWord, 0, sizeof(g_state->newWord));
                    memset(g_state->newMeaning, 0, sizeof(g_state->newMeaning));
                    memset(g_state->newPronunciation, 0, sizeof(g_state->newPronunciation));
                }
            }
            // Ensure child region is properly closed
            ImGui::EndChild();
        }
        
        // 单词列表区域
        ImGui::Spacing();
        if (ImGui::CollapsingHeader("📚 单词列表", ImGuiTreeNodeFlags_DefaultOpen))
        {
            // 导入/导出按钮栏
#ifdef _WIN32
            {
                if (ImGui::Button("导出..."))
                {
                    std::wstring savePath;
                    if (ShowSaveFileDialog(savePath))
                    {
                        ExportWordsToPath(savePath);
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("导入..."))
                {
                    std::wstring openPath;
                    if (ShowOpenFileDialog(openPath))
                    {
                        if (ImportWordsFromPath(openPath))
                        {
                            // 成功导入后，数据和统计已更新
                        }
                    }
                }
            }
#endif

            if (g_state->words.empty())
            {
                ImGui::TextDisabled("还没有添加任何单词");
            }
            else
            {
                // 创建排序索引：已掌握的单词排在最后面
                std::vector<int> sortedIndices(g_state->words.size());
                for (int i = 0; i < static_cast<int>(g_state->words.size()); i++)
                {
                    sortedIndices[i] = i;
                }
                
                // 排序：已掌握的单词排在最后面
                std::sort(sortedIndices.begin(), sortedIndices.end(), 
                    [&](int a, int b) {
                        bool aMastered = g_state->words[a].isMastered;
                        bool bMastered = g_state->words[b].isMastered;
                        if (aMastered != bMastered)
                        {
                            return !aMastered; // 未掌握的排在前面
                        }
                        return a < b; // 相同状态下保持原有顺序
                    });
                
                for (int idx = 0; idx < static_cast<int>(sortedIndices.size()); idx++)
                {
                    int i = sortedIndices[idx];
                    const auto& entry = g_state->words[i];
                    
                    ImGui::PushID(i);
                    
                    // 检查是否过期需要复习
                    auto now = std::chrono::system_clock::now();
                    bool isDue = entry.isActive && !entry.isMastered && entry.remindTime <= now;
                    
                    // 显示掌握状态
                    if (entry.isMastered)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.8f, 0.2f, 1.0f));
                        ImGui::Text("✅ 已掌握");
                        ImGui::PopStyleColor();
                    }
                    else if (isDue)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                        ImGui::Text("需要复习");
                        ImGui::PopStyleColor();
                    }
                    else
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
                        ImGui::Text("学习中");
                        ImGui::PopStyleColor();
                    }
                    
                    ImGui::SameLine();
                    {
                        std::string id = std::string("##word_") + std::to_string(i);
                        DrawCopyableText(id.c_str(), entry.word);
                    }
                    
                    // 已移除音标显示
                    
                    ImGui::TextWrapped("释义:");
                    {
                        std::string idm = std::string("##meaning_") + std::to_string(i);
                        DrawCopyableMultiline(idm.c_str(), entry.meaning);
                    }
                    
                    if (entry.isMastered)
                    {
                        ImGui::Text("复习次数: %d | 状态: 已掌握", entry.reviewCount);
                    }
                    else
                    {
                        ImGui::Text("复习次数: %d | 下次提醒: %s", 
                                   entry.reviewCount, 
                                   TimeUntilNow(entry.remindTime).c_str());
                    }
                    
                    // 操作按钮
                    if (isDue)
                    {
                        ImGui::SameLine();
                        if (ImGui::Button("标记已复习"))
                        {
                            MarkAsReviewed(i);
                        }
                    }
                    
                    // 掌握状态按钮
                    if (entry.isMastered)
                    {
                        ImGui::SameLine();
                        if (ImGui::Button("取消掌握"))
                        {
                            UnmarkAsMastered(i);
                        }
                    }
                    else
                    {
                        ImGui::SameLine();
                        if (ImGui::Button("标记已掌握"))
                        {
                            MarkAsMastered(i);
                        }
                    }
                    
                    ImGui::SameLine();
                    if (ImGui::Button("编辑"))
                    {
                        g_state->selectedWordIndex = i;
                        g_state->isEditing = true;
                        strncpy(g_state->editWord, entry.word.c_str(), sizeof(g_state->editWord));
                        g_state->editWord[sizeof(g_state->editWord)-1] = '\0';
                        strncpy(g_state->editMeaning, entry.meaning.c_str(), sizeof(g_state->editMeaning));
                        g_state->editMeaning[sizeof(g_state->editMeaning)-1] = '\0';
                    }
                    
                    ImGui::SameLine();
                    if (ImGui::Button("删除"))
                    {
                        RemoveWord(i);
                        ImGui::PopID();
                        continue;
                    }
                    
                    // 只有未掌握的单词才显示提醒按钮
                    if (!entry.isMastered)
                    {
                        ImGui::SameLine();
                        if (ImGui::Button("5秒后提醒"))
                        {
                            auto& e = g_state->words[i];
                            e.remindTime = std::chrono::system_clock::now() + std::chrono::seconds(5);
                            SaveWords();
                        }
                    }
                    
                    // 内联编辑区域
                    if (g_state->isEditing && g_state->selectedWordIndex == i)
                    {
                        ImGui::Spacing();
                        ImGui::TextDisabled("编辑:");
                        ImGui::InputText("单词", g_state->editWord, sizeof(g_state->editWord));
                        ImGui::InputTextMultiline("释义", g_state->editMeaning, sizeof(g_state->editMeaning), ImVec2(-1, 100));
                        if (ImGui::Button("保存"))
                        {
                            auto& e = g_state->words[i];
                            e.word = g_state->editWord;
                            e.meaning = g_state->editMeaning;
                            SaveWords();
                            g_state->isEditing = false;
                            g_state->selectedWordIndex = -1;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("取消"))
                        {
                            g_state->isEditing = false;
                            g_state->selectedWordIndex = -1;
                        }
                    }

                    ImGui::Separator();
                    ImGui::PopID();
                }
            }
        }
        
        // 编辑弹窗
        if (g_state->selectedWordIndex >= 0 && g_state->selectedWordIndex < (int)g_state->words.size())
        {
            ImGui::SetNextWindowSize(ImVec2(460, 260), ImGuiCond_Appearing);
            if (ImGui::BeginPopupModal("编辑单词", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                auto& entry = g_state->words[g_state->selectedWordIndex];
                static char editWord[256];
                static char editPron[256];
                static char editMeaning[512];
                static bool initialized = false;
                if (!initialized || ImGui::IsWindowAppearing())
                {
                    strncpy(editWord, entry.word.c_str(), sizeof(editWord)); editWord[sizeof(editWord)-1] = '\0';
                    strncpy(editPron, entry.pronunciation.c_str(), sizeof(editPron)); editPron[sizeof(editPron)-1] = '\0';
                    strncpy(editMeaning, entry.meaning.c_str(), sizeof(editMeaning)); editMeaning[sizeof(editMeaning)-1] = '\0';
                    initialized = true;
                }

                ImGui::InputText("单词", editWord, sizeof(editWord));
                ImGui::InputText("音标", editPron, sizeof(editPron));
                ImGui::InputTextMultiline("释义", editMeaning, sizeof(editMeaning), ImVec2(420, 120));

                ImGui::Separator();
                if (ImGui::Button("保存", ImVec2(200, 0)))
                {
                    entry.word = editWord;
                    entry.pronunciation = editPron;
                    entry.meaning = editMeaning;
                    SaveWords();
                    g_state->selectedWordIndex = -1;
                    initialized = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("取消", ImVec2(200, 0)))
                {
                    g_state->selectedWordIndex = -1;
                    initialized = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

        // 检查是否需要显示提醒窗口 - 进一步减少检查频率
        static auto lastCheckTime = std::chrono::steady_clock::now();
        static bool lastHasReminder = false;
        static std::vector<WordEntry> lastDueWords; // 保存上次检查的单词列表
        auto now = std::chrono::steady_clock::now();
        
        // 每1秒检查一次，避免过于频繁的检查
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastCheckTime).count() >= 1000)
        {
            auto currentDueWords = GetDueWords();
            bool currentHasReminder = !currentDueWords.empty();
            
            // 检查单词列表是否真正改变
            bool wordsChanged = false;
            if (currentDueWords.size() != lastDueWords.size())
            {
                wordsChanged = true;
            }
            else
            {
                // 比较每个单词的内容
                for (size_t i = 0; i < currentDueWords.size(); ++i)
                {
                    if (i >= lastDueWords.size() || 
                        currentDueWords[i].word != lastDueWords[i].word ||
                        currentDueWords[i].meaning != lastDueWords[i].meaning)
                    {
                        wordsChanged = true;
                        break;
                    }
                }
            }
            
            // 只有当状态真正改变时才更新
            if (currentHasReminder != lastHasReminder || wordsChanged)
            {
                g_state->showReminderPopup = currentHasReminder;
                lastHasReminder = currentHasReminder;
                lastDueWords = currentDueWords;
            }
            
            lastCheckTime = now;
        }
        
        // 系统级提醒通知 - Windows右上角原生弹窗
        if (g_state->showReminderPopup)
        {
#ifdef _WIN32
            EnsureReminderWindow();
            
            // 如果窗口创建失败或者没有需要复习的单词，重置标志
            if (!g_reminderHwnd && !HasReminderToShow())
            {
                g_state->showReminderPopup = false;
                lastHasReminder = false;
            }
#else
            // 非Windows平台暂不支持系统级弹窗，回退到关闭标志
            g_state->showReminderPopup = false;
            lastHasReminder = false;
#endif
        }
        
        ImGui::End();
    }
}
