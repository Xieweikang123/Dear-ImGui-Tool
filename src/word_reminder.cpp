#include "word_reminder.h"
#include "imgui.h"
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
#include <dwmapi.h>
#pragma comment(lib, "Dwmapi.lib")
#endif
#include <algorithm>

namespace WordReminder
{
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
        int reminderSeconds = 1800; // 默认30分钟
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
        int defaultReminderSeconds = 1800; // 默认30分钟
        
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
    
    // 保存单词到文件
    void SaveWords()
    {
        std::ofstream file("word_reminder_data.txt");
        if (file.is_open())
        {
            for (const auto& entry : g_state->words)
            {
                file << entry.word << "|"
                     << entry.meaning << "|"
                     << entry.pronunciation << "|"
                     << std::chrono::system_clock::to_time_t(entry.remindTime) << "|"
                     << entry.isActive << "|"
                     << entry.reviewCount << "|"
                     << std::chrono::system_clock::to_time_t(entry.lastReview) << "\n";
            }
            file.close();
        }
    }
    
    // 从文件加载单词
    void LoadWords()
    {
        std::ifstream file("word_reminder_data.txt");
        if (file.is_open())
        {
            std::string line;
            while (std::getline(file, line))
            {
                std::istringstream iss(line);
                std::string word, meaning, pronunciation;
                time_t remindTime, lastReview;
                bool isActive;
                int reviewCount;
                
                std::getline(iss, word, '|');
                std::getline(iss, meaning, '|');
                std::getline(iss, pronunciation, '|');
                iss >> remindTime;
                iss.ignore(); // 跳过分隔符
                iss >> isActive;
                iss.ignore(); // 跳过分隔符
                iss >> reviewCount;
                iss.ignore(); // 跳过分隔符
                iss >> lastReview;
                
                WordEntry entry;
                entry.word = word;
                entry.meaning = meaning;
                entry.pronunciation = pronunciation;
                entry.remindTime = std::chrono::system_clock::from_time_t(remindTime);
                entry.isActive = isActive;
                entry.reviewCount = reviewCount;
                entry.lastReview = std::chrono::system_clock::from_time_t(lastReview);
                
                g_state->words.push_back(entry);
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
            if (entry.isActive && entry.remindTime <= now)
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
    static HFONT g_fontTitle = nullptr;
    static HFONT g_fontText = nullptr;
    static HFONT g_fontButton = nullptr;
    static bool g_darkMode = false;
    static BYTE g_animOpacity = 0; // 0-255，用于淡入

    enum ReminderCmdIds { BTN_REVIEWED = 1001, BTN_SNOOZE = 1002, BTN_CLOSE = 1003 };

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
        int w3 = std::max<int>(110, IdealButtonWidth(L"关闭"));
        int btnHeight = 34, gap = 12;
        int totalWidth = w1 + w2 + w3 + gap * 2;
        int startX = rc.right - totalWidth - 16;
        int y = rc.bottom - btnHeight - 14;
        HWND btnReviewed = GetDlgItem(hwnd, BTN_REVIEWED);
        HWND btnSnooze = GetDlgItem(hwnd, BTN_SNOOZE);
        HWND btnClose = GetDlgItem(hwnd, BTN_CLOSE);
        if (btnReviewed) MoveWindow(btnReviewed, startX, y, w1, btnHeight, TRUE);
        if (btnSnooze)   MoveWindow(btnSnooze,   startX + w1 + gap, y, w2, btnHeight, TRUE);
        if (btnClose)    MoveWindow(btnClose,    startX + w1 + gap + w2 + gap, y, w3, btnHeight, TRUE);
    }

    static LRESULT CALLBACK ReminderWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
            case WM_CREATE:
            {
                // Fonts
                if (!g_fontTitle)
                {
                    g_fontTitle = CreateFontW(22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                }
                if (!g_fontText)
                {
                    g_fontText = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                }
                if (!g_fontButton)
                {
                    g_fontButton = CreateFontW(16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                }

                HWND b1 = CreateWindowW(L"BUTTON", L"标记已复习",
                              WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                              0, 0, 0, 0, hwnd, (HMENU)BTN_REVIEWED, GetModuleHandleW(nullptr), nullptr);
                HWND b2 = CreateWindowW(L"BUTTON", L"稍后提醒",
                              WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                              0, 0, 0, 0, hwnd, (HMENU)BTN_SNOOZE, GetModuleHandleW(nullptr), nullptr);
                HWND b3 = CreateWindowW(L"BUTTON", L"关闭",
                              WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                              0, 0, 0, 0, hwnd, (HMENU)BTN_CLOSE, GetModuleHandleW(nullptr), nullptr);
                if (b1) SendMessageW(b1, WM_SETFONT, (WPARAM)g_fontButton, TRUE);
                if (b2) SendMessageW(b2, WM_SETFONT, (WPARAM)g_fontButton, TRUE);
                if (b3) SendMessageW(b3, WM_SETFONT, (WPARAM)g_fontButton, TRUE);
                LayoutButtons(hwnd);
                SetTimer(hwnd, 1, 15000, nullptr); // 15秒自动关闭

                // 应用系统主题及DWM效果
                g_darkMode = IsSystemDarkMode();
                ApplyDwmWindowAttributes(hwnd, g_darkMode);

                // 初始透明并淡入
                g_animOpacity = 0;
                SetLayeredWindowAttributes(hwnd, 0, g_animOpacity, LWA_ALPHA);
                SetTimer(hwnd, 2, 15, nullptr); // 每15ms 提升透明度
                return 0;
            }
            case WM_SIZE:
            {
                LayoutButtons(hwnd);
                return 0;
            }
            case WM_COMMAND:
            {
                int id = LOWORD(wParam);
                if (id == BTN_REVIEWED)
                {
                    MarkAllDueReviewed();
                    g_state->showReminderPopup = false;
                    DestroyWindow(hwnd);
                    return 0;
                }
                if (id == BTN_SNOOZE)
                {
                    SnoozeAllDueFiveMinutes();
                    g_state->showReminderPopup = false;
                    DestroyWindow(hwnd);
                    return 0;
                }
                if (id == BTN_CLOSE)
                {
                    g_state->showReminderPopup = false;
                    DestroyWindow(hwnd);
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
                // 背景
                COLORREF clrWnd = g_darkMode ? RGB(32, 32, 36) : RGB(248, 249, 251);
                HBRUSH bgWnd = CreateSolidBrush(clrWnd);
                FillRect(hdc, &rc, bgWnd);
                DeleteObject(bgWnd);

                // 内容卡片
                RECT content = { rc.left + 16, rc.top + 16, rc.right - 16, rc.bottom - 64 };
                COLORREF clrCard = g_darkMode ? RGB(43, 43, 48) : RGB(255, 255, 255);
                COLORREF clrBorder = g_darkMode ? RGB(64, 64, 72) : RGB(230, 234, 238);
                HBRUSH brCard = CreateSolidBrush(clrCard);
                HPEN pnCard = CreatePen(PS_SOLID, 1, clrBorder);
                HGDIOBJ oldPen = SelectObject(hdc, pnCard);
                HGDIOBJ oldBrush = SelectObject(hdc, brCard);
                RoundRect(hdc, content.left, content.top, content.right, content.bottom, 10, 10);
                SelectObject(hdc, oldBrush);
                SelectObject(hdc, oldPen);
                DeleteObject(brCard);
                DeleteObject(pnCard);

                // 左侧色条强调
                HBRUSH brAccent = CreateSolidBrush(RGB(45, 140, 255));
                RECT accent = { content.left, content.top, content.left + 4, content.bottom };
                FillRect(hdc, &accent, brAccent);
                DeleteObject(brAccent);

                // 标题
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, g_darkMode ? RGB(240, 240, 240) : RGB(28, 28, 30));
                if (g_fontTitle) SelectObject(hdc, g_fontTitle);
                RECT titleRc = { content.left + 12, content.top + 10, content.right - 12, content.top + 40 };
                DrawTextW(hdc, L"提醒", -1, &titleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                // 正文
                if (g_fontText) SelectObject(hdc, g_fontText);
                SetTextColor(hdc, g_darkMode ? RGB(220, 220, 225) : RGB(60, 60, 68));
                RECT textRc = { content.left + 12, content.top + 44, content.right - 12, content.bottom - 12 };
                DrawTextW(hdc, g_reminderText.c_str(), -1, &textRc, DT_LEFT | DT_TOP | DT_WORDBREAK);

                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_DRAWITEM:
            {
                LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lParam;
                if (!dis) break;
                bool pressed = (dis->itemState & ODS_SELECTED) != 0;

                // 主要按钮（标记已复习）使用实心蓝色，其余为浅色填充+边框
                bool isPrimary = (GetDlgCtrlID(dis->hwndItem) == BTN_REVIEWED);
                COLORREF primary = RGB(45, 140, 255);
                COLORREF primaryPressed = RGB(29, 112, 214);
                COLORREF fill = g_darkMode ? RGB(58, 58, 64) : RGB(245, 247, 250);
                COLORREF border = g_darkMode ? RGB(80, 80, 88) : RGB(220, 224, 228);
                if (isPrimary) fill = pressed ? primaryPressed : primary;

                HBRUSH b = CreateSolidBrush(fill);
                HPEN p = CreatePen(PS_SOLID, 1, isPrimary ? RGB(30, 118, 224) : border);
                HGDIOBJ oldB = SelectObject(dis->hDC, b);
                HGDIOBJ oldP = SelectObject(dis->hDC, p);
                RoundRect(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom, 8, 8);
                SelectObject(dis->hDC, oldB);
                SelectObject(dis->hDC, oldP);
                DeleteObject(b);
                DeleteObject(p);
                SetBkMode(dis->hDC, TRANSPARENT);
                COLORREF txt = isPrimary ? RGB(255,255,255) : (g_darkMode ? RGB(230,230,235) : RGB(40,40,44));
                SetTextColor(dis->hDC, txt);
                if (g_fontButton) SelectObject(dis->hDC, g_fontButton);
                wchar_t buf[128];
                GetWindowTextW(dis->hwndItem, buf, 128);
                DrawTextW(dis->hDC, buf, -1, (RECT*)&dis->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }
            case WM_KEYDOWN:
            {
                if (wParam == VK_ESCAPE)
                {
                    g_state->showReminderPopup = false;
                    DestroyWindow(hwnd);
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
                    DestroyWindow(hwnd);
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
                DestroyWindow(hwnd);
                return 0;
            }
            case WM_DESTROY:
            {
                if (hwnd == g_reminderHwnd) g_reminderHwnd = nullptr;
                if (g_fontTitle) { DeleteObject(g_fontTitle); g_fontTitle = nullptr; }
                if (g_fontText) { DeleteObject(g_fontText); g_fontText = nullptr; }
                if (g_fontButton) { DeleteObject(g_fontButton); g_fontButton = nullptr; }
                return 0;
            }
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    static void EnsureReminderWindow()
    {
        if (g_reminderHwnd) return;

        auto dueWords = GetDueWords();
        if (dueWords.empty()) { g_state->showReminderPopup = false; return; }

        std::ostringstream oss;
        oss << "🔔 提醒\n\n";
        for (const auto& entry : dueWords)
        {
            oss << "📖 " << entry.word << "\n";
            if (!entry.pronunciation.empty()) oss << "    [" << entry.pronunciation << "]\n";
            oss << "    " << entry.meaning << "\n\n";
        }
        g_reminderText = Utf8ToWide(oss.str());

        WNDCLASSW wc = {};
        wc.lpfnWndProc = ReminderWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"WordReminderPopupWindow";
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.style = CS_DROPSHADOW;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        static ATOM atom = RegisterClassW(&wc);
        (void)atom;

        // 根据内容自适应窗口尺寸
        int baseWidth = 380;
        int baseHeight = 220;
        SIZE contentSize = {0,0};
        {
            HDC hdc = GetDC(nullptr);
            HFONT old = nullptr;
            if (g_fontText) old = (HFONT)SelectObject(hdc, g_fontText);
            RECT rcMeasure = {0,0,480,1000};
            DrawTextW(hdc, g_reminderText.c_str(), -1, &rcMeasure, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_CALCRECT);
            contentSize.cx = rcMeasure.right - rcMeasure.left;
            contentSize.cy = rcMeasure.bottom - rcMeasure.top;
            if (old) SelectObject(hdc, old);
            ReleaseDC(nullptr, hdc);
        }
        int width = std::max<int>(baseWidth, static_cast<int>(contentSize.cx) + 16 + 16 + 24);
        int height = std::max<int>(baseHeight, static_cast<int>(contentSize.cy) + 16 + 16 + 64 + 50);

        // Ensure width fits all buttons
        int w1 = std::max<int>(110, IdealButtonWidth(L"标记已复习"));
        int w2 = std::max<int>(110, IdealButtonWidth(L"稍后提醒"));
        int w3 = std::max<int>(110, IdealButtonWidth(L"关闭"));
        int buttonsTotal = w1 + w2 + w3 + 12 * 2 + 16 + 16; // gaps + side margins
        width = std::max<int>(width, buttonsTotal);

        RECT workArea;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
        int x = workArea.right - width - 20;
        int y = workArea.top + 20;

        g_reminderHwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
            wc.lpszClassName,
            L"提醒",
            WS_CAPTION | WS_SYSMENU,
            x, y, width, height,
            nullptr, nullptr, wc.hInstance, nullptr);

        if (g_reminderHwnd)
        {
            ShowWindow(g_reminderHwnd, SW_SHOWNORMAL);
            UpdateWindow(g_reminderHwnd);
        }
        else
        {
            // 回退：无法创建窗口则不再显示
            g_state->showReminderPopup = false;
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
    
    bool HasReminderToShow()
    {
        if (!g_state || !g_state->autoShowReminders) return false;
        
        // 如果已经显示提醒窗口，就不再重复显示
        if (g_state->showReminderPopup) return false;
        
        auto now = std::chrono::system_clock::now();
        for (const auto& entry : g_state->words)
        {
            if (entry.isActive && entry.remindTime <= now)
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
            if (entry.isActive && entry.remindTime <= now)
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
        if (!ImGui::Begin("单词学习提醒", &g_state->windowOpen))
        {
            ImGui::End();
            return;
        }
        
        // 统计信息区域
        ImGui::BeginChild("Stats", ImVec2(0, 80), true);
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "📊 学习统计");
        ImGui::Separator();
        
        ImGui::Columns(4, "stats");
        ImGui::Text("总单词数: %d", g_state->totalWords);
        ImGui::NextColumn();
        ImGui::Text("今日复习: %d", g_state->reviewedToday);
        ImGui::NextColumn();
        ImGui::Text("待复习: %d", g_state->dueWords);
        ImGui::NextColumn();
        ImGui::Text("活跃单词: %d", g_state->dueWords);
        ImGui::Columns(1);
        ImGui::EndChild();
        
        // 添加新单词区域
        ImGui::Spacing();
        if (ImGui::CollapsingHeader("➕ 添加新单词", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::BeginChild("AddWord", ImVec2(0, 200), true);
            
            ImGui::Columns(2, "add_word");
            ImGui::SetColumnWidth(0, 150);
            
            ImGui::Text("单词:");
            ImGui::SameLine();
            ImGui::InputText("##Word", g_state->newWord, sizeof(g_state->newWord));
            
            // 已移除音标输入
            ImGui::NextColumn();
            
            ImGui::NextColumn();
            ImGui::Text("释义:");
            ImGui::SameLine();
            ImGui::InputTextMultiline("##Meaning", g_state->newMeaning, sizeof(g_state->newMeaning));
            
                         ImGui::NextColumn();
             
             // 提醒时间设置区域
             ImGui::Text("提醒时间:");
             ImGui::SameLine();
             
             // 快速预设按钮
             ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
             
             const char* presets[] = {"5秒","30秒", "1分钟", "5分钟", "10分钟", "15分钟", "30分钟", "1小时", "2小时", "4小时"};
             int presetSeconds[] = {5,30, 60, 300, 600, 900, 1800, 3600, 7200, 14400};
             
             for (int i = 0; i < 10; i++)
             {
                 if (i > 0) ImGui::SameLine();
                 
                 bool isSelected = (g_state->reminderSeconds == presetSeconds[i]);
                 if (isSelected)
                 {
                     ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 1.0f, 1.0f));
                     ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.7f, 1.0f, 1.0f));
                 }
                 
                 if (ImGui::Button(presets[i], ImVec2(60, 24)))
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
            
            ImGui::Columns(1);
            
            ImGui::EndChild();
            
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
        }
        
        // 单词列表区域
        ImGui::Spacing();
        if (ImGui::CollapsingHeader("📚 单词列表", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::BeginChild("WordList", ImVec2(0, 300), true);
            
            if (g_state->words.empty())
            {
                ImGui::TextDisabled("还没有添加任何单词");
            }
            else
            {
                for (int i = 0; i < static_cast<int>(g_state->words.size()); i++)
                {
                    const auto& entry = g_state->words[i];
                    
                    ImGui::PushID(i);
                    
                    // 检查是否过期需要复习
                    auto now = std::chrono::system_clock::now();
                    bool isDue = entry.isActive && entry.remindTime <= now;
                    
                    if (isDue)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                        ImGui::Text("⚠️ 需要复习");
                        ImGui::PopStyleColor();
                    }
                    
                    ImGui::SameLine();
                    ImGui::Text("%s", entry.word.c_str());
                    
                    // 已移除音标显示
                    
                    ImGui::TextWrapped("释义: %s", entry.meaning.c_str());
                    
                    ImGui::Text("复习次数: %d | 下次提醒: %s", 
                               entry.reviewCount, 
                               TimeUntilNow(entry.remindTime).c_str());
                    
                    // 操作按钮
                    if (isDue)
                    {
                        ImGui::SameLine();
                        if (ImGui::Button("标记已复习"))
                        {
                            MarkAsReviewed(i);
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
                    
                    ImGui::SameLine();
                    if (ImGui::Button("5秒后提醒"))
                    {
                        auto& e = g_state->words[i];
                        e.remindTime = std::chrono::system_clock::now() + std::chrono::seconds(5);
                        SaveWords();
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
            
            ImGui::EndChild();
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

        // 检查是否需要显示提醒
        if (HasReminderToShow())
        {
            g_state->showReminderPopup = true;
        }
        
        // 系统级提醒通知 - Windows右上角原生弹窗
        if (g_state->showReminderPopup)
        {
#ifdef _WIN32
            EnsureReminderWindow();
#else
            // 非Windows平台暂不支持系统级弹窗，回退到关闭标志
            g_state->showReminderPopup = false;
#endif
        }
        
        ImGui::End();
    }
}
