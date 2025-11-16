#include "ollama_client.h"
#include "replace_tool.h"
#include <string>
#include <thread>
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
    
    // 调用 Ollama/OpenAI 兼容 API
    static std::string CallOllamaChat(const std::string& hostUtf8,
                                      int port,
                                      const std::string& pathUtf8,
                                      const std::string& modelName,
                                      const std::string& userContent)
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
            AppendLog("[Ollama] WinHttpOpen failed");
            return std::string();
        }

        DWORD timeout = 5000; // 5s connect/send/receive timeout
        WinHttpSetTimeouts(hSession, timeout, timeout, timeout, timeout);

        HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), static_cast<INTERNET_PORT>(port), 0);
        if (!hConnect)
        {
            AppendLog("[Ollama] WinHttpConnect failed");
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
            AppendLog("[Ollama] WinHttpOpenRequest failed");
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return std::string();
        }

        std::string jsonBody;
        jsonBody.reserve(512 + userContent.size());
        jsonBody = "{\"model\":\"" + model + "\",\"messages\":[";
        jsonBody += "{\"role\":\"system\",\"content\":\"You are a helpful English-Chinese word explanation assistant. Reply in Chinese only, and DO NOT use markdown or any formatting symbols. Output plain text only.\"},";
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
            AppendLog("[Ollama] WinHttpSendRequest failed");
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return std::string();
        }

        bResults = WinHttpReceiveResponse(hRequest, NULL);
        if (!bResults)
        {
            AppendLog("[Ollama] WinHttpReceiveResponse failed");
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return std::string();
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

        const std::string key = "\"content\":";
        size_t pos = response.find(key);
        if (pos == std::string::npos)
        {
            AppendLog("[Ollama] no content field in response");
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
#endif

    std::string GenerateWordMeaning(const std::string& word)
    {
#ifdef _WIN32
        std::string prompt = std::string("请用中文解释这个英文单词，并给 1-2 个简单例句(例句是纯英文版，不要翻译成中文)，不要使用 Markdown 或任何格式符号：") + word;
        return CallOllamaChat(g_config.host, g_config.port, g_config.path, g_config.model, prompt);
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
        std::string testResult = CallOllamaChat(g_config.host, g_config.port, g_config.path, g_config.model, "test");
        return !testResult.empty();
#else
        return false;
#endif
    }
}

