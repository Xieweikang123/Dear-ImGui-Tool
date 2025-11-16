#pragma once

#include <string>
#include <vector>
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
        
        // 超时设置（毫秒）
        int connectTimeout = 10000;      // 连接超时 10秒
        int sendTimeout = 30000;         // 发送超时 30秒
        int receiveTimeout = 60000;      // 接收超时 60秒
        
        // 重试设置
        int maxRetries = 3;              // 最大重试次数
        int retryDelayMs = 1000;         // 重试延迟（毫秒），每次重试会指数增加
        
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
    
    // 同步调用 Ollama API 生成单词故事
    // words: 待复习的单词列表
    // 返回：成功时返回生成的故事，失败时返回空字符串
    std::string GenerateStoryFromWords(const std::vector<std::string>& words);
    
    // 异步调用 Ollama API 生成单词故事
    // words: 待复习的单词列表
    // callback: (success, result) -> void
    void GenerateStoryFromWordsAsync(const std::vector<std::string>& words,
                                     std::function<void(bool, const std::string&)> callback);
}

