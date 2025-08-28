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
#include <windows.h>
#endif

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

    // 右上角自定义置顶弹窗
    static HWND g_reminderHwnd = nullptr;
    static std::wstring g_reminderText;

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

    static LRESULT CALLBACK ReminderWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
            case WM_CREATE:
            {
                CreateWindowW(L"BUTTON", L"标记已复习",
                              WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                              20, 140, 90, 28, hwnd, (HMENU)BTN_REVIEWED, GetModuleHandleW(nullptr), nullptr);
                CreateWindowW(L"BUTTON", L"稍后提醒",
                              WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                              120, 140, 90, 28, hwnd, (HMENU)BTN_SNOOZE, GetModuleHandleW(nullptr), nullptr);
                CreateWindowW(L"BUTTON", L"关闭",
                              WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                              220, 140, 60, 28, hwnd, (HMENU)BTN_CLOSE, GetModuleHandleW(nullptr), nullptr);
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

                HBRUSH bg = CreateSolidBrush(RGB(255, 153, 51));
                FillRect(hdc, &rc, bg);
                DeleteObject(bg);

                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(255, 255, 255));

                RECT textRc = { 16, 12, rc.right - 16, 130 };
                DrawTextW(hdc, g_reminderText.c_str(), -1, &textRc, DT_LEFT | DT_TOP | DT_WORDBREAK);

                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_DESTROY:
            {
                if (hwnd == g_reminderHwnd) g_reminderHwnd = nullptr;
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
        oss << "🔔 单词复习提醒\n\n";
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
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        static ATOM atom = RegisterClassW(&wc);
        (void)atom;

        int width = 300;
        int height = 180;

        RECT workArea;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
        int x = workArea.right - width - 20;
        int y = workArea.top + 20;

        g_reminderHwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            wc.lpszClassName,
            L"单词复习提醒",
            WS_POPUP | WS_CAPTION | WS_MINIMIZEBOX,
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
            ImGui::BeginChild("AddWord", ImVec2(0, 0), true);
            
            ImGui::Columns(2, "add_word");
            ImGui::SetColumnWidth(0, 150);
            
            ImGui::Text("单词:");
            ImGui::SameLine();
            ImGui::InputText("##Word", g_state->newWord, sizeof(g_state->newWord));
            
            ImGui::NextColumn();
            ImGui::Text("音标:");
            ImGui::SameLine();
            ImGui::InputText("##Pronunciation", g_state->newPronunciation, sizeof(g_state->newPronunciation));
            
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
             ImGui::Text("自定义时间:");
             
             // 自定义时间输入
             ImGui::Columns(3, "custom_time");
             ImGui::SetColumnWidth(0, 80);
             ImGui::SetColumnWidth(1, 80);
             ImGui::SetColumnWidth(2, 80);
             
             static int hours = 0, minutes = 30, seconds = 0;
             
             // 同步显示当前设置的时间
             if (ImGui::IsWindowAppearing())
             {
                 hours = g_state->reminderSeconds / 3600;
                 minutes = (g_state->reminderSeconds % 3600) / 60;
                 seconds = g_state->reminderSeconds % 60;
             }
             
             ImGui::Text("时:");
             ImGui::SameLine();
             if (ImGui::InputInt("##Hours", &hours, 1, 5, ImGuiInputTextFlags_CharsDecimal))
             {
                 if (hours < 0) hours = 0;
                 if (hours > 23) hours = 23;
                 g_state->reminderSeconds = hours * 3600 + minutes * 60 + seconds;
             }
             
             ImGui::NextColumn();
             ImGui::Text("分:");
             ImGui::SameLine();
             if (ImGui::InputInt("##Minutes", &minutes, 1, 5, ImGuiInputTextFlags_CharsDecimal))
             {
                 if (minutes < 0) minutes = 0;
                 if (minutes > 59) minutes = 59;
                 g_state->reminderSeconds = hours * 3600 + minutes * 60 + seconds;
             }
             
             ImGui::NextColumn();
             ImGui::Text("秒:");
             ImGui::SameLine();
             if (ImGui::InputInt("##Seconds", &seconds, 1, 5, ImGuiInputTextFlags_CharsDecimal))
             {
                 if (seconds < 0) seconds = 0;
                 if (seconds > 59) seconds = 59;
                 g_state->reminderSeconds = hours * 3600 + minutes * 60 + seconds;
             }
             
             ImGui::Columns(1);
             
             // 显示总时间
             ImGui::Spacing();
             ImGui::TextDisabled("总计: %d小时%d分钟%d秒", 
                                g_state->reminderSeconds / 3600,
                                (g_state->reminderSeconds % 3600) / 60,
                                g_state->reminderSeconds % 60);
            
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
                    
                    if (!entry.pronunciation.empty())
                    {
                        ImGui::SameLine();
                        ImGui::TextDisabled("[%s]", entry.pronunciation.c_str());
                    }
                    
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
                    }
                    
                    ImGui::SameLine();
                    if (ImGui::Button("删除"))
                    {
                        RemoveWord(i);
                        ImGui::PopID();
                        continue;
                    }
                    
                    ImGui::Separator();
                    ImGui::PopID();
                }
            }
            
            ImGui::EndChild();
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
