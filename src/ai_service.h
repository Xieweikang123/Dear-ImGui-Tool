#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace AIService
{
    // AI 配置结构
    struct AIConfig
    {
        std::string apiAddress = "http://localhost:11434";  // Ollama 默认地址
        std::string model = "llama3.2";  // 默认模型
        std::string apiKey = "";  // 可选 API 密钥
        bool enabled = false;  // 是否启用 AI 功能
        
        AIConfig() {}
    };
    
    // 初始化 AI 服务
    void Initialize();
    
    // 清理 AI 服务
    void Cleanup();
    
    // 获取配置
    AIConfig GetConfig();
    
    // 设置配置
    void SetConfig(const AIConfig& config);
    
    // 加载配置（从文件）
    void LoadConfig();
    
    // 保存配置（到文件）
    void SaveConfig();
    
    // 生成单词释义（异步）
    // callback: (success, result) -> void
    void GenerateWordMeaning(const std::string& word, 
                            std::function<void(bool, const std::string&)> callback);
    
    // 测试 API 连接
    bool TestConnection();
    
    // 获取可用模型列表（异步）
    void GetAvailableModels(std::function<void(bool, const std::vector<std::string>&)> callback);
}

