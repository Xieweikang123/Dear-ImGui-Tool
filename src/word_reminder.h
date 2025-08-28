#pragma once

#include <string>
#include <vector>
#include <chrono>

namespace WordReminder
{
    // 单词条目结构
    struct WordEntry
    {
        std::string word;
        std::string meaning;
        std::string pronunciation;
        std::chrono::system_clock::time_point remindTime;
        bool isActive;
        int reviewCount;
        std::chrono::system_clock::time_point lastReview;
        
        WordEntry() : isActive(true), reviewCount(0) {}
    };
    
    // 初始化功能模块
    void Initialize();
    
    // 清理功能模块
    void Cleanup();
    
    // 绘制UI
    void DrawUI();
    
    // 获取功能名称
    const char* GetFeatureName();
    
    // 检查功能是否启用
    bool IsEnabled();
    
    // 启用/禁用功能
    void SetEnabled(bool enabled);
    
    // 添加新单词
    void AddWord(const std::string& word, const std::string& meaning, int secondsFromNow = 1800);
    
    // 删除单词
    void RemoveWord(int index);
    
    // 标记单词为已复习
    void MarkAsReviewed(int index);
    
    // 检查是否有需要提醒的单词
    bool HasReminderToShow();
    
    // 获取需要提醒的单词列表
    std::vector<WordEntry> GetDueWords();
}
