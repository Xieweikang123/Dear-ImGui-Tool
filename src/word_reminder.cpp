#include "word_reminder.h"
#include "imgui.h"
#include <string>
#include <vector>
#include <memory>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>

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
            ImGui::BeginChild("AddWord", ImVec2(0, 120), true);
            
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
            ImGui::Text("提醒时间:");
            ImGui::SameLine();
            ImGui::SliderInt("##Seconds", &g_state->reminderSeconds, 10, 86400, "%d 秒后");
            
            // 显示更友好的时间格式
            int minutes = g_state->reminderSeconds / 60;
            int seconds = g_state->reminderSeconds % 60;
            if (minutes > 0)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(%d分%d秒)", minutes, seconds);
            }
            
            ImGui::Columns(1);
            
            if (ImGui::Button("添加单词", ImVec2(-1, 0)))
            {
                if (strlen(g_state->newWord) > 0 && strlen(g_state->newMeaning) > 0)
                {
                    AddWord(g_state->newWord, g_state->newMeaning, g_state->reminderSeconds);
                    
                    // 清空输入
                    memset(g_state->newWord, 0, sizeof(g_state->newWord));
                    memset(g_state->newMeaning, 0, sizeof(g_state->newMeaning));
                    memset(g_state->newPronunciation, 0, sizeof(g_state->newPronunciation));
                }
            }
            
            ImGui::EndChild();
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
        
        // 提醒弹窗
        if (g_state->showReminderPopup && HasReminderToShow())
        {
            ImGui::OpenPopup("单词复习提醒");
            g_state->showReminderPopup = false;
        }
        
        if (ImGui::BeginPopupModal("单词复习提醒", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("🔔 有单词需要复习了！");
            ImGui::Separator();
            
            auto dueWords = GetDueWords();
            for (const auto& entry : dueWords)
            {
                ImGui::Text("📖 %s", entry.word.c_str());
                ImGui::Text("   %s", entry.meaning.c_str());
                ImGui::Separator();
            }
            
            if (ImGui::Button("我知道了", ImVec2(120, 0)))
            {
                ImGui::CloseCurrentPopup();
            }
            
            ImGui::EndPopup();
        }
        
        ImGui::End();
    }
}
