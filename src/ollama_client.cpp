#include "ollama_client.h"
#include "replace_tool.h"
#include <string>
#include <thread>
#include <chrono>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "Winhttp.lib")
#endif

namespace OllamaClient
{
    using ReplaceTool::AppendLog;
    
    static OllamaConfig g_config;
    
    void Initialize()
    {
        // 使用默认配置
    }
    
    void Cleanup()
    {
        // 目前无需清理资源
    }
    
    OllamaConfig GetConfig()
    {
        return g_config;
    }
    
    void SetConfig(const OllamaConfig& config)
    {
        g_config = config;
    }
    
    void SetConfig(const char* host, int port, const char* path, const char* model)
    {
        g_config.host = host ? host : "121.129.32.42";
        g_config.port = port > 0 ? port : 11434;
        g_config.path = path ? path : "/v1/chat/completions";
        g_config.model = model ? model : "gpt-oss:20b";
    }
    
#ifdef _WIN32
    // UTF-8 转宽字符
    static std::wstring Utf8ToWide(const std::string& utf8)
    {
        if (utf8.empty()) return std::wstring();
        int count = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
        if (count <= 0) return std::wstring();
        std::wstring wide;
        wide.resize(count - 1);
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), count);
        return wide;
    }
    
    // 清理 AI 返回的文本（去除 markdown 符号等）
    static std::string CleanAiText(const std::string& result)
    {
        std::string cleaned;
        cleaned.reserve(result.size());
        bool atLineStart = true;
        for (size_t i = 0; i < result.size(); ++i)
        {
            char c = result[i];
            if (atLineStart)
            {
                if (c == ' ' || c == '\t')
                    continue;
                if (c == '-' && i + 1 < result.size() && result[i + 1] == ' ')
                {
                    ++i;
                    continue;
                }
                if (c == '*' || c == '#')
                    continue;
            }
            if (c == '*')
                continue;
            cleaned.push_back(c);
            atLineStart = (c == '\n' || c == '\r');
        }
        return cleaned;
    }
    
    // 调用 Ollama/OpenAI 兼容 API（内部实现，不带重试）
    static std::string CallOllamaChatInternal(const std::string& hostUtf8,
                                              int port,
                                              const std::string& pathUtf8,
                                              const std::string& modelName,
                                              const std::string& userContent,
                                              const std::string& systemContent = "")
    {
        std::wstring host = Utf8ToWide(hostUtf8.empty() ? g_config.host : hostUtf8);
        std::wstring path = Utf8ToWide(pathUtf8.empty() ? g_config.path : pathUtf8);
        if (host.empty()) host = Utf8ToWide(g_config.host);
        if (path.empty()) path = Utf8ToWide(g_config.path);
        if (port <= 0) port = g_config.port;
        std::string model = modelName.empty() ? g_config.model : modelName;

        HINTERNET hSession = WinHttpOpen(L"Dear-ImGui-Tool/1.0",
                                         WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                         WINHTTP_NO_PROXY_NAME,
                                         WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession)
        {
            DWORD error = GetLastError();
            AppendLog("[Ollama] WinHttpOpen failed, error=" + std::to_string(error));
            return std::string();
        }

        // 使用配置的超时时间
        WinHttpSetTimeouts(hSession, 
                          g_config.connectTimeout,
                          g_config.sendTimeout,
                          g_config.receiveTimeout,
                          g_config.receiveTimeout);

        HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), static_cast<INTERNET_PORT>(port), 0);
        if (!hConnect)
        {
            DWORD error = GetLastError();
            AppendLog("[Ollama] WinHttpConnect failed, host=" + hostUtf8 + ":" + std::to_string(port) + 
                     ", error=" + std::to_string(error));
            WinHttpCloseHandle(hSession);
            return std::string();
        }

        HINTERNET hRequest = WinHttpOpenRequest(hConnect,
                                               L"POST",
                                               path.c_str(),
                                               NULL,
                                               WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               0);
        if (!hRequest)
        {
            DWORD error = GetLastError();
            AppendLog("[Ollama] WinHttpOpenRequest failed, path=" + pathUtf8 + 
                     ", error=" + std::to_string(error));
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return std::string();
        }

        std::string jsonBody;
        jsonBody.reserve(512 + userContent.size() + systemContent.size());
        jsonBody = "{\"model\":\"" + model + "\",\"messages\":[";
        
        // 如果没有指定 system content，使用默认的中文助手
        std::string sysContent = systemContent.empty() ? 
            "You are a helpful English-Chinese word explanation assistant. Reply in Chinese only, and DO NOT use markdown or any formatting symbols. Output plain text only." :
            systemContent;
        
        jsonBody += "{\"role\":\"system\",\"content\":\"";
        for (char c : sysContent)
        {
            if (c == '\\') jsonBody += "\\\\";
            else if (c == '"') jsonBody += "\\\"";
            else if (c == '\n') jsonBody += "\\n";
            else jsonBody.push_back(c);
        }
        jsonBody += "\"},";
        jsonBody += "{\"role\":\"user\",\"content\":\"";
        for (char c : userContent)
        {
            if (c == '\\') jsonBody += "\\\\";
            else if (c == '"') jsonBody += "\\\"";
            else if (c == '\n') jsonBody += "\\n";
            else jsonBody.push_back(c);
        }
        jsonBody += "\"}],\"temperature\":0.7}";

        std::wstring headers = L"Content-Type: application/json\r\n";
        BOOL bResults = WinHttpSendRequest(hRequest,
                                           headers.c_str(),
                                           (DWORD)headers.size(),
                                           (LPVOID)jsonBody.data(),
                                           (DWORD)jsonBody.size(),
                                           (DWORD)jsonBody.size(),
                                           0);
        if (!bResults)
        {
            DWORD error = GetLastError();
            AppendLog("[Ollama] WinHttpSendRequest failed, error=" + std::to_string(error));
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return std::string();
        }

        bResults = WinHttpReceiveResponse(hRequest, NULL);
        if (!bResults)
        {
            DWORD error = GetLastError();
            AppendLog("[Ollama] WinHttpReceiveResponse failed, error=" + std::to_string(error));
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return std::string();
        }
        
        // 检查 HTTP 状态码
        DWORD statusCode = 0;
        DWORD statusCodeSize = sizeof(statusCode);
        if (WinHttpQueryHeaders(hRequest, 
                                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                NULL, &statusCode, &statusCodeSize, NULL))
        {
            if (statusCode != 200)
            {
                AppendLog("[Ollama] HTTP status code=" + std::to_string(statusCode));
                WinHttpCloseHandle(hRequest);
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                return std::string();
            }
        }

        std::string response;
        DWORD dwSize = 0;
        do
        {
            dwSize = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &dwSize) || dwSize == 0)
                break;

            std::string buffer;
            buffer.resize(dwSize);
            DWORD dwDownloaded = 0;
            if (!WinHttpReadData(hRequest, &buffer[0], dwSize, &dwDownloaded) || dwDownloaded == 0)
                break;
            buffer.resize(dwDownloaded);
            response.append(buffer);
        } while (dwSize > 0);

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);

        if (response.empty())
        {
            AppendLog("[Ollama] 响应为空");
            return std::string();
        }
        
        const std::string key = "\"content\":";
        size_t pos = response.find(key);
        if (pos == std::string::npos)
        {
            AppendLog("[Ollama] 响应中没有 content 字段，响应长度=" + std::to_string(response.size()));
            if (response.size() < 500) // 只记录短响应，避免日志过长
            {
                AppendLog("[Ollama] 响应内容: " + response.substr(0, 500));
            }
            return std::string();
        }
        pos += key.size();
        while (pos < response.size() && (response[pos] == ' ' || response[pos] == '\n'))
            ++pos;
        if (pos >= response.size() || response[pos] != '"')
        {
            AppendLog("[Ollama] content field malformed");
            return std::string();
        }
        ++pos;
        std::string content;
        while (pos < response.size())
        {
            char c = response[pos++];
            if (c == '\\')
            {
                if (pos >= response.size()) break;
                char esc = response[pos++];
                if (esc == 'n') content.push_back('\n');
                else if (esc == 't') content.push_back('\t');
                else content.push_back(esc);
            }
            else if (c == '"')
            {
                break;
            }
            else
            {
                content.push_back(c);
            }
        }

        return CleanAiText(content);
    }
    
    // 调用 Ollama/OpenAI 兼容 API（带重试机制）
    static std::string CallOllamaChatWithRetry(const std::string& hostUtf8,
                                               int port,
                                               const std::string& pathUtf8,
                                               const std::string& modelName,
                                               const std::string& userContent)
    {
        int retryCount = 0;
        int maxRetries = g_config.maxRetries;
        int baseDelay = g_config.retryDelayMs;
        
        while (retryCount <= maxRetries)
        {
            std::string result = CallOllamaChatInternal(hostUtf8, port, pathUtf8, modelName, userContent, "");
            
            // 如果成功或超过最大重试次数，返回结果
            if (!result.empty() || retryCount >= maxRetries)
            {
                if (result.empty() && retryCount >= maxRetries)
                {
                    AppendLog("[Ollama] 请求失败，已达最大重试次数(" + std::to_string(maxRetries) + ")");
                }
                return result;
            }
            
            // 重试前等待（指数退避）
            retryCount++;
            if (retryCount <= maxRetries)
            {
                int delay = baseDelay * (1 << (retryCount - 1)); // 指数退避：1s, 2s, 4s...
                AppendLog("[Ollama] 请求失败，第 " + std::to_string(retryCount) + " 次重试，等待 " + 
                         std::to_string(delay) + " 毫秒...");
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            }
        }
        
        return std::string();
    }
#endif

    std::string GenerateWordMeaning(const std::string& word)
    {
#ifdef _WIN32
        std::string prompt = std::string("请用中文解释这个英文单词，并给 1-2 个简单例句(例句是纯英文版，不要翻译成中文)，不要使用 Markdown 或任何格式符号：") + word;
        return CallOllamaChatWithRetry(g_config.host, g_config.port, g_config.path, g_config.model, prompt);
#else
        // 非 Windows 平台暂不支持
        return std::string();
#endif
    }
    
    void GenerateWordMeaningAsync(const std::string& word,
                                  std::function<void(bool, const std::string&)> callback)
    {
        if (!callback) return;
        
        std::thread([word, callback]()
        {
            std::string result = GenerateWordMeaning(word);
            bool success = !result.empty();
            callback(success, result);
        }).detach();
    }
    
    bool TestConnection()
    {
#ifdef _WIN32
        std::string testResult = CallOllamaChatWithRetry(g_config.host, g_config.port, g_config.path, g_config.model, "test");
        return !testResult.empty();
#else
        return false;
#endif
    }
    
    std::string GenerateStoryFromWords(const std::vector<std::string>& words)
    {
#ifdef _WIN32
        if (words.empty())
        {
            return std::string();
        }
        
        // 构建提示词：要求 AI 用这些单词编一个连贯的英文故事
        std::string prompt = "Please write a short, coherent, and interesting English story (about 200-300 words) using the following English words. ";
        prompt += "You must use all the words and mark each word with 【】when it first appears. Word list:\n";
        
        for (size_t i = 0; i < words.size(); ++i)
        {
            prompt += words[i];
            if (i < words.size() - 1)
            {
                prompt += ", ";
            }
        }
        
        prompt += "\n\nRequirements:\n";
        prompt += "1. The story should be fluent and natural with a complete plot\n";
        prompt += "2. Mark each word with 【word】when it first appears\n";
        prompt += "3. Use plain English text only, no Markdown formatting\n";
        prompt += "4. The story should be interesting and memorable";
        
        // 使用英文 system prompt 生成英文故事
        std::string systemPrompt = "You are a creative English story writer. Write stories in English only, and DO NOT use markdown or any formatting symbols. Output plain English text only.";
        
        // 直接调用内部函数以使用自定义 system prompt
        int retryCount = 0;
        int maxRetries = g_config.maxRetries;
        int baseDelay = g_config.retryDelayMs;
        
        while (retryCount <= maxRetries)
        {
            std::string result = CallOllamaChatInternal(g_config.host, g_config.port, g_config.path, g_config.model, prompt, systemPrompt);
            
            if (!result.empty() || retryCount >= maxRetries)
            {
                if (result.empty() && retryCount >= maxRetries)
                {
                    AppendLog("[Ollama] 故事生成失败，已达最大重试次数(" + std::to_string(maxRetries) + ")");
                }
                return result;
            }
            
            retryCount++;
            if (retryCount <= maxRetries)
            {
                int delay = baseDelay * (1 << (retryCount - 1));
                AppendLog("[Ollama] 故事生成失败，第 " + std::to_string(retryCount) + " 次重试，等待 " + 
                         std::to_string(delay) + " 毫秒...");
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            }
        }
        
        return std::string();
#else
        // 非 Windows 平台暂不支持
        return std::string();
#endif
    }
    
    void GenerateStoryFromWordsAsync(const std::vector<std::string>& words,
                                     std::function<void(bool, const std::string&)> callback)
    {
        if (!callback) return;
        
        std::thread([words, callback]()
        {
            std::string result = GenerateStoryFromWords(words);
            bool success = !result.empty();
            callback(success, result);
        }).detach();
    }
}

