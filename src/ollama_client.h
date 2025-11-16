#pragma once

#include <string>
#include <functional>

#ifdef _WIN32
#include <windows.h>
#endif

namespace OllamaClient
{
    // Ollama 配置结构
    struct OllamaConfig
    {
        std::string host = "121.129.32.42";
        int port = 11434;
        std::string path = "/v1/chat/completions";
        std::string model = "gpt-oss:20b";
        
        OllamaConfig() {}
    };
    
    // 初始化 Ollama 客户端
    void Initialize();
    
    // 清理 Ollama 客户端
    void Cleanup();
    
    // 获取配置
    OllamaConfig GetConfig();
    
    // 设置配置
    void SetConfig(const OllamaConfig& config);
    
    // 从参数设置配置（用于兼容旧的字符数组配置）
    void SetConfig(const char* host, int port, const char* path, const char* model);
    
    // 同步调用 Ollama API 生成单词释义
    // 返回：成功时返回生成的释义，失败时返回空字符串
    std::string GenerateWordMeaning(const std::string& word);
    
    // 异步调用 Ollama API 生成单词释义
    // callback: (success, result) -> void
    void GenerateWordMeaningAsync(const std::string& word,
                                  std::function<void(bool, const std::string&)> callback);
    
    // 测试连接
    bool TestConnection();
}

