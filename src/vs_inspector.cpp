#include "vs_inspector.h"
#include "replace_tool.h"

#include "imgui.h"
#include <string>
#include <vector>
#include <mutex>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <cstring>
#include <ctime>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <unordered_map>
#include <unordered_set>
#include <objbase.h>
#include <oleauto.h>
#include <shlobj.h>
#include <filesystem>
#include <commdlg.h>
#include <atlbase.h>
#include <atlcom.h>
#pragma comment(lib, "Comdlg32.lib")
namespace fs = std::filesystem;
#endif

namespace VSInspector
{
#ifdef _WIN32
    using ReplaceTool::AppendLog;

    struct VSInstance
    {
        DWORD pid = 0;
        std::string exePath;
        std::string windowTitle;
        std::string solutionPath;
        std::string activeDocumentPath;
    };

    struct CursorInstance
    {
        DWORD pid = 0;
        std::string exePath;
        std::string windowTitle;
        std::string folderPath;
        std::string workspaceName;
    };

    static std::vector<VSInstance> g_vsList;
    static std::vector<CursorInstance> g_cursorList;
    static std::string g_feishuPath;
    static bool g_feishuRunning = false;
    static std::string g_wechatPath;
    static bool g_wechatRunning = false;
    static std::string g_currentWechatPath;  // 当前检测到的微信路径
    static std::mutex g_vsMutexVS;

    // Configuration structure
    struct SavedConfig
    {
        std::string name;
        std::string vsSolutionPath;
        std::string cursorFolderPath;
        std::string feishuPath;
        std::string wechatPath;
        unsigned long long createdAt = 0;    // unix time seconds
        unsigned long long lastUsedAt = 0;   // unix time seconds
    };

    // Selections and persistence
    static std::vector<SavedConfig> g_savedConfigs;
    static std::string g_selectedSlnPath;
    static std::unordered_set<std::string> g_selectedSlnPaths;
    static std::string g_selectedCursorFolder;
    static std::unordered_set<std::string> g_selectedCursorFolders;
    static std::string g_currentConfigName;
    static bool g_prefsLoaded = false;  // Track if prefs have been loaded

             // 用于主界面配置名称输入框的全局变量
    static char g_mainConfigNameBuf[256] = {0};
    static bool g_shouldFillConfigName = false;
    
    // 自动刷新相关变量
    static bool g_autoRefreshEnabled = true;
    static float g_lastRefreshTime = 0.0f;
    static const float g_autoRefreshInterval = 5.0f; // 5秒间隔
    
    // 启动动画相关变量
    static bool g_showStartupAnimation = true;
    static float g_startupAnimationTime = 0.0f;
    static const float g_startupAnimationDuration = 1.0f; // 2秒动画时长，增加科技感
    static int g_startupAnimationStep = 0;
    static const char* g_startupAnimationTexts[] = {
        "🚀 INITIALIZING DEVELOPMENT ENVIRONMENT MANAGER...",
        "🔍 SCANNING RUNNING APPLICATIONS...",
        "⚙️ LOADING CONFIGURATION DATA...",
        "✨ SYSTEM READY!"
    };
    
    // 科技感动画变量
    static float g_scanLineY = 0.0f;
    static float g_particleTime = 0.0f;
    static float g_dataStreamTime = 0.0f;
    static float g_glitchTime = 0.0f;
    static int g_glitchCounter = 0;
    


    // Forward declare env helper used by prefs
    static std::string GetEnvU8(const char* name);
    
    // System resource monitoring
    struct SystemResources {
        float cpuUsage = 0.0f;
        unsigned long long totalMemory = 0;
        unsigned long long usedMemory = 0;
        unsigned long long totalDisk = 0;
        unsigned long long freeDisk = 0;
        unsigned long long uptime = 0;
    };
    
    static SystemResources g_systemResources;
    static float g_lastResourceUpdate = 0.0f;
    static const float g_resourceUpdateInterval = 2.0f; // 2秒更新一次
    

    
    // Get system resources
    static void UpdateSystemResources()
    {
        // CPU Usage (simplified - using GetTickCount64 for demo)
        static ULONGLONG lastTickCount = 0;
        ULONGLONG currentTickCount = GetTickCount64();
        if (lastTickCount > 0) {
            g_systemResources.cpuUsage = 50.0f + 20.0f * sinf(ImGui::GetTime() * 0.5f); // 模拟CPU使用率
        }
        lastTickCount = currentTickCount;
        
        // Memory Info
        MEMORYSTATUSEX memInfo;
        memInfo.dwLength = sizeof(MEMORYSTATUSEX);
        if (GlobalMemoryStatusEx(&memInfo)) {
            g_systemResources.totalMemory = memInfo.ullTotalPhys;
            g_systemResources.usedMemory = memInfo.ullTotalPhys - memInfo.ullAvailPhys;
        }
        
        // Disk Info (C: drive)
        ULARGE_INTEGER freeBytesAvailable, totalBytes, totalFreeBytes;
        if (GetDiskFreeSpaceExA("C:\\", &freeBytesAvailable, &totalBytes, &totalFreeBytes)) {
            g_systemResources.totalDisk = totalBytes.QuadPart;
            g_systemResources.freeDisk = totalFreeBytes.QuadPart;
        }
        
        // System Uptime
        g_systemResources.uptime = GetTickCount64() / 1000; // 转换为秒
    }
    
    // Forward declare launch helpers
    static bool LaunchVSWithSolution(const std::string& slnPath);
    static bool LaunchCursorWithFolder(const std::string& folderPath);
    static bool LaunchFeishu();
    static bool LaunchWechat();

    // Generic process detection helper
    static bool DetectProcessAndGetPath(const std::string& exeLower, const std::vector<std::string>& processNames, 
                                       DWORD processId, std::string& outPath, bool& outRunning, const std::string& logPrefix)
    {
        for (const auto& name : processNames)
        {
            if (exeLower == name)
            {
                HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, processId);
                if (hProc)
                {
                    char buf[MAX_PATH];
                    DWORD sz = (DWORD)sizeof(buf);
                    if (QueryFullProcessImageNameA(hProc, 0, buf, &sz))
                        outPath.assign(buf, sz);
                    CloseHandle(hProc);
                }
                outRunning = true;
                // 进程检测日志已删除
                return true;
            }
        }
        return false;
    }

    // File dialog helpers (ANSI)
    static bool ShowOpenFileDialog(char* outPath, size_t outSize, const char* filter, const char* title)
    {
        OPENFILENAMEA ofn = {0};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = NULL;
        ofn.lpstrFilter = filter; // e.g., "JSON Files\0*.json\0All Files\0*.*\0\0"
        ofn.lpstrFile = outPath;
        ofn.nMaxFile = (DWORD)outSize;
        ofn.lpstrTitle = title;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
        return GetOpenFileNameA(&ofn) == TRUE;
    }
    static bool ShowSaveFileDialog(char* outPath, size_t outSize, const char* filter, const char* title, const char* defExt)
    {
        OPENFILENAMEA ofn = {0};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = NULL;
        ofn.lpstrFilter = filter;
        ofn.lpstrFile = outPath;
        ofn.nMaxFile = (DWORD)outSize;
        ofn.lpstrTitle = title;
        ofn.lpstrDefExt = defExt; // e.g., "json"
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
        return GetSaveFileNameA(&ofn) == TRUE;
    }

    static fs::path GetPrefsFile()
    {
        std::string appdata = GetEnvU8("APPDATA");
        fs::path dir = appdata.empty() ? fs::path(".") : fs::path(appdata);
        dir /= "DearImGuiTool";
        std::error_code ec; fs::create_directories(dir, ec);
        return dir / "prefs.txt";
    }

    static fs::path GetPrefsJsonFile()
    {
        std::string appdata = GetEnvU8("APPDATA");
        fs::path dir = appdata.empty() ? fs::path(".") : fs::path(appdata);
        dir /= "DearImGuiTool";
        std::error_code ec; fs::create_directories(dir, ec);
        return dir / "prefs.json";
    }

    static fs::path GetDefaultExportJsonFile()
    {
        // Prefer Desktop if available; otherwise use DearImGuiTool folder
        std::string userProfile = GetEnvU8("USERPROFILE");
        fs::path base = userProfile.empty() ? GetPrefsJsonFile().parent_path() : fs::path(userProfile) / "Desktop";
        std::error_code ec; if (!fs::exists(base, ec)) base = GetPrefsJsonFile().parent_path();
        // timestamp
        std::time_t t = std::time(nullptr);
        std::tm tmv; localtime_s(&tmv, &t);
        char buf[64]; strftime(buf, sizeof(buf), "DearImGuiTool-configs-%Y%m%d-%H%M%S.json", &tmv);
        return base / buf;
    }

    static std::string JsonEscape(const std::string& s)
    {
        std::string out; out.reserve(s.size() + 8);
        for (char c : s)
        {
            switch (c)
            {
                case '\\': out += "\\\\"; break;
                case '"': out += "\\\""; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default: out += c; break;
            }
        }
        return out;
    }

    // Forward declarations for legacy txt load/save
    static void SavePrefsToTxt();
    static bool LoadPrefsFromTxt();

    static void SaveConfigsToJsonFile(const fs::path& filePath)
    {
        // Ensure current selection is reflected in g_savedConfigs
        if (!g_currentConfigName.empty())
        {
            bool found = false;
            for (auto& config : g_savedConfigs)
            {
                if (config.name == g_currentConfigName)
                {
                    config.vsSolutionPath = g_selectedSlnPath;
                    config.cursorFolderPath = g_selectedCursorFolder;
                    config.feishuPath = g_feishuPath;
                    config.wechatPath = g_wechatPath;
                    if (config.createdAt == 0) config.createdAt = (unsigned long long)time(nullptr);
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                SavedConfig newConfig;
                newConfig.name = g_currentConfigName;
                newConfig.vsSolutionPath = g_selectedSlnPath;
                newConfig.cursorFolderPath = g_selectedCursorFolder;
                newConfig.feishuPath = g_feishuPath;
                newConfig.wechatPath = g_wechatPath;
                newConfig.createdAt = (unsigned long long)time(nullptr);
                g_savedConfigs.push_back(newConfig);
            }
        }

        fs::path p = filePath;
        std::ofstream ofs(p.string(), std::ios::binary);
        if (!ofs) { AppendLog(std::string("[prefs] open for write failed: ") + p.string()); return; }
        ofs << "{\n  \"configs\": [\n";
        for (size_t i = 0; i < g_savedConfigs.size(); ++i)
        {
            const auto& c = g_savedConfigs[i];
            ofs << "    {\n";
            ofs << "      \"name\": \"" << JsonEscape(c.name) << "\",\n";
            ofs << "      \"vs\": \"" << JsonEscape(c.vsSolutionPath) << "\",\n";
            ofs << "      \"cursor\": \"" << JsonEscape(c.cursorFolderPath) << "\",\n";
            ofs << "      \"feishu\": \"" << JsonEscape(c.feishuPath) << "\",\n";
            ofs << "      \"wechat\": \"" << JsonEscape(c.wechatPath) << "\",\n";
            ofs << "      \"createdAt\": " << c.createdAt << ",\n";
            ofs << "      \"lastUsedAt\": " << c.lastUsedAt << "\n";
            ofs << "    }" << (i + 1 < g_savedConfigs.size() ? ",\n" : "\n");
        }
        ofs << "  ]\n}";
        AppendLog(std::string("[prefs] saved JSON ") + std::to_string(g_savedConfigs.size()) + " config(s) to " + p.string());
        if (!g_selectedSlnPath.empty()) AppendLog("[prefs] saved VS solution: " + g_selectedSlnPath);
        if (!g_selectedCursorFolder.empty()) AppendLog("[prefs] saved Cursor folder: " + g_selectedCursorFolder);
        if (!g_feishuPath.empty()) AppendLog("[prefs] saved Feishu path: " + g_feishuPath);
        if (!g_wechatPath.empty()) AppendLog("[prefs] saved WeChat path: " + g_wechatPath);
    }

    static void SavePrefsToJson()
    {
        SaveConfigsToJsonFile(GetPrefsJsonFile());
    }

    static bool ParseJsonString(const std::string& s, size_t& pos, std::string& out)
    {
        out.clear();
        if (pos >= s.size() || s[pos] != '"') return false;
        pos++;
        while (pos < s.size())
        {
            char c = s[pos++];
            if (c == '"') return true;
            if (c == '\\' && pos < s.size())
            {
                char e = s[pos++];
                switch (e) { case 'n': out += '\n'; break; case 'r': out += '\r'; break; case 't': out += '\t'; break; case '"': out += '"'; break; case '\\': out += '\\'; break; default: out += e; break; }
            }
            else { out += c; }
        }
        return false;
    }

    static void SkipWs(const std::string& s, size_t& pos) { while (pos < s.size() && (s[pos]==' '||s[pos]=='\n'||s[pos]=='\r'||s[pos]=='\t')) pos++; }

    static bool ParseConfigsFromJson(const std::string& content, std::vector<SavedConfig>& out)
    {
        out.clear();
        size_t pos = 0; SkipWs(content, pos);
        if (pos >= content.size() || content[pos] != '{') { return false; }
        pos++; // '{'
        bool ok = false;
        while (pos < content.size())
        {
            SkipWs(content, pos);
            if (pos < content.size() && content[pos] == '}') { pos++; break; }
            // parse key
            std::string key; if (!ParseJsonString(content, pos, key)) break;
            SkipWs(content, pos); if (pos >= content.size() || content[pos] != ':') break; pos++;
            SkipWs(content, pos);
            if (key == "configs")
            {
                if (pos >= content.size() || content[pos] != '[') break; pos++;
                SkipWs(content, pos);
                while (pos < content.size() && content[pos] != ']')
                {
                    SkipWs(content, pos);
                    if (pos >= content.size() || content[pos] != '{') break; pos++;
                    SavedConfig c;
                    while (pos < content.size())
                    {
                        SkipWs(content, pos);
                        if (pos < content.size() && content[pos] == '}') { pos++; break; }
                        std::string k; if (!ParseJsonString(content, pos, k)) { pos = content.size(); break; }
                        SkipWs(content, pos); if (pos >= content.size() || content[pos] != ':') { pos = content.size(); break; } pos++;
                        SkipWs(content, pos);
                                                 if (k == "name" || k == "vs" || k == "cursor" || k == "feishu" || k == "wechat")
                         {
                             std::string v; if (!ParseJsonString(content, pos, v)) { pos = content.size(); break; }
                             if (k == "name") c.name = v; else if (k == "vs") c.vsSolutionPath = v; else if (k == "cursor") c.cursorFolderPath = v; else if (k == "feishu") c.feishuPath = v; else c.wechatPath = v;
                         }
                        else if (k == "createdAt" || k == "lastUsedAt")
                        {
                            size_t start = pos; while (pos < content.size() && (isdigit((unsigned char)content[pos]) || content[pos]=='-')) pos++; unsigned long long val = strtoull(content.substr(start, pos-start).c_str(), nullptr, 10);
                            if (k == "createdAt") c.createdAt = val; else c.lastUsedAt = val;
                        }
                        SkipWs(content, pos);
                        if (pos < content.size() && content[pos] == ',') { pos++; }
                    }
                    if (!c.name.empty()) out.push_back(c);
                    SkipWs(content, pos);
                    if (pos < content.size() && content[pos] == ',') { pos++; }
                    SkipWs(content, pos);
                }
                if (pos < content.size() && content[pos] == ']') { pos++; ok = true; }
            }
            SkipWs(content, pos);
            if (pos < content.size() && content[pos] == ',') { pos++; continue; }
        }
        return ok;
    }

    static bool LoadPrefsFromJson()
    {
        g_savedConfigs.clear();
        fs::path p = GetPrefsJsonFile();
        std::error_code ec; if (!fs::exists(p, ec)) { return false; }
        std::ifstream ifs(p.string(), std::ios::binary);
        if (!ifs) { AppendLog(std::string("[prefs] open for read failed: ") + p.string()); return false; }
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        bool ok = ParseConfigsFromJson(content, g_savedConfigs);
        if (ok) AppendLog(std::string("[prefs] loaded JSON ") + std::to_string(g_savedConfigs.size()) + " config(s) from " + p.string());
        return ok;
    }

    static void SavePrefsToTxt()
    {
        // Save current selection to the current config
        if (!g_currentConfigName.empty())
        {
            // Find existing config or create new one
            bool found = false;
            for (auto& config : g_savedConfigs)
            {
                if (config.name == g_currentConfigName)
                {
                    config.vsSolutionPath = g_selectedSlnPath;
                    config.cursorFolderPath = g_selectedCursorFolder;
                    if (config.createdAt == 0) config.createdAt = (unsigned long long)time(nullptr);
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                SavedConfig newConfig;
                newConfig.name = g_currentConfigName;
                newConfig.vsSolutionPath = g_selectedSlnPath;
                newConfig.cursorFolderPath = g_selectedCursorFolder;
                newConfig.createdAt = (unsigned long long)time(nullptr);
                g_savedConfigs.push_back(newConfig);
            }
        }

        // Save all configs to file
        fs::path p = GetPrefsFile();
        std::ofstream ofs(p.string(), std::ios::binary);
        if (!ofs) { AppendLog(std::string("[prefs] open for write failed: ") + p.string()); return; }
        
        for (const auto& config : g_savedConfigs)
        {
                         ofs << "config=" << config.name << "\n";
             ofs << "sln=" << config.vsSolutionPath << "\n";
             ofs << "cursor=" << config.cursorFolderPath << "\n";
             ofs << "feishu=" << config.feishuPath << "\n";
             ofs << "wechat=" << config.wechatPath << "\n";
             ofs << "created=" << config.createdAt << "\n";
             ofs << "used=" << config.lastUsedAt << "\n";
            ofs << "---\n";  // Separator between configs
        }
        
                 AppendLog(std::string("[prefs] saved ") + std::to_string(g_savedConfigs.size()) + " config(s) to " + p.string());
         if (!g_selectedSlnPath.empty())
             AppendLog("[prefs] saved VS solution: " + g_selectedSlnPath);
         if (!g_selectedCursorFolder.empty())
             AppendLog("[prefs] saved Cursor folder: " + g_selectedCursorFolder);
         if (!g_feishuPath.empty())
             AppendLog("[prefs] saved Feishu path: " + g_feishuPath);
         if (!g_wechatPath.empty())
             AppendLog("[prefs] saved WeChat path: " + g_wechatPath);
    }

    static bool LoadPrefsFromTxt()
    {
        g_savedConfigs.clear();
        fs::path p = GetPrefsFile();
        std::error_code ec; if (!fs::exists(p, ec)) { AppendLog("[prefs] no prefs file"); return false; }
        std::ifstream ifs(p.string(), std::ios::binary);
        if (!ifs) { AppendLog(std::string("[prefs] open for read failed: ") + p.string()); return false; }
        
        std::string line;
        SavedConfig currentConfig;
        bool inConfig = false;
        
        while (std::getline(ifs, line))
        {
            if (line == "---")
            {
                // End of config, save it
                if (inConfig && !currentConfig.name.empty())
                {
                    g_savedConfigs.push_back(currentConfig);
                }
                currentConfig = SavedConfig();
                inConfig = false;
            }
            else if (line.rfind("config=", 0) == 0)
            {
                currentConfig.name = line.substr(7);
                inConfig = true;
            }
            else if (line.rfind("sln=", 0) == 0)
            {
                currentConfig.vsSolutionPath = line.substr(4);
            }
                         else if (line.rfind("cursor=", 0) == 0)
             {
                 currentConfig.cursorFolderPath = line.substr(7);
             }
             else if (line.rfind("feishu=", 0) == 0)
             {
                 currentConfig.feishuPath = line.substr(7);
             }
             else if (line.rfind("wechat=", 0) == 0)
             {
                 currentConfig.wechatPath = line.substr(7);
             }
            else if (line.rfind("created=", 0) == 0)
            {
                currentConfig.createdAt = strtoull(line.substr(8).c_str(), nullptr, 10);
            }
            else if (line.rfind("used=", 0) == 0)
            {
                currentConfig.lastUsedAt = strtoull(line.substr(5).c_str(), nullptr, 10);
            }
        }
        
        // Don't forget the last config if no separator
        if (inConfig && !currentConfig.name.empty())
        {
            g_savedConfigs.push_back(currentConfig);
        }
        
        AppendLog(std::string("[prefs] loaded ") + std::to_string(g_savedConfigs.size()) + " config(s) from " + p.string());
        return true;
    }

    static void SavePrefs()
    {
        // Prefer JSON; also write legacy txt for backward compatibility
        SavePrefsToJson();
        SavePrefsToTxt();
    }

    static void LoadPrefs()
    {
        // Try JSON first; fall back to txt
        if (!LoadPrefsFromJson())
        {
            if (!LoadPrefsFromTxt())
            {
                AppendLog("[prefs] no prefs found in JSON or TXT");
            }
        }
        g_prefsLoaded = true;
    }

    static void LoadConfig(const std::string& configName)
    {
        for (const auto& config : g_savedConfigs)
        {
            if (config.name == configName)
            {
                                 g_selectedSlnPath = config.vsSolutionPath;
                 g_selectedCursorFolder = config.cursorFolderPath;
                 g_feishuPath = config.feishuPath;
                 g_wechatPath = config.wechatPath;
                 // Keep the multi-select sets in sync with single selection
                 g_selectedSlnPaths.clear();
                 if (!g_selectedSlnPath.empty())
                     g_selectedSlnPaths.insert(g_selectedSlnPath);
                 g_selectedCursorFolders.clear();
                 if (!g_selectedCursorFolder.empty())
                     g_selectedCursorFolders.insert(g_selectedCursorFolder);
                 g_currentConfigName = configName;
                AppendLog("[prefs] loaded config: " + configName);
                                 if (!g_selectedSlnPath.empty())
                     AppendLog("[prefs] loaded VS solution: " + g_selectedSlnPath);
                 if (!g_selectedCursorFolder.empty())
                     AppendLog("[prefs] loaded Cursor folder: " + g_selectedCursorFolder);
                 if (!g_feishuPath.empty())
                     AppendLog("[prefs] loaded Feishu path: " + g_feishuPath);
                 if (!g_wechatPath.empty())
                     AppendLog("[prefs] loaded WeChat path: " + g_wechatPath);
                return;
            }
        }
        AppendLog("[prefs] config not found: " + configName);
    }

    static void DeleteConfig(const std::string& configName)
    {
        for (auto it = g_savedConfigs.begin(); it != g_savedConfigs.end(); ++it)
        {
            if (it->name == configName)
            {
                g_savedConfigs.erase(it);
                // If we deleted the current config, clear the current config name
                if (g_currentConfigName == configName)
                {
                    g_currentConfigName.clear();
                }
                // Save the updated configs to file
                SavePrefs();
                AppendLog("[prefs] deleted config: " + configName);
                return;
            }
        }
        AppendLog("[prefs] config not found for deletion: " + configName);
    }

    // Merge helpers for import
    static void MergeConfigs(std::vector<SavedConfig>& into, const std::vector<SavedConfig>& incoming)
    {
        for (const auto& inc : incoming)
        {
            if (inc.name.empty()) continue;
            bool found = false;
            for (auto& cur : into)
            {
                if (cur.name == inc.name)
                {
                    found = true;
                    // Merge rule: keep earliest createdAt, max lastUsedAt, overwrite paths if provided
                    if (cur.createdAt == 0 || (inc.createdAt != 0 && inc.createdAt < cur.createdAt)) cur.createdAt = inc.createdAt;
                    if (inc.lastUsedAt > cur.lastUsedAt) cur.lastUsedAt = inc.lastUsedAt;
                                         if (!inc.vsSolutionPath.empty()) cur.vsSolutionPath = inc.vsSolutionPath;
                     if (!inc.cursorFolderPath.empty()) cur.cursorFolderPath = inc.cursorFolderPath;
                     if (!inc.feishuPath.empty()) cur.feishuPath = inc.feishuPath;
                     if (!inc.wechatPath.empty()) cur.wechatPath = inc.wechatPath;
                    break;
                }
            }
            if (!found)
            {
                into.push_back(inc);
            }
        }
    }

    

    static void EnsurePrefsLoaded()
    {
        if (!g_prefsLoaded)
        {
            LoadPrefs();
        }
    }

    static bool LaunchVSWithSolution(const std::string& slnPath)
    {
        if (slnPath.empty()) return false;
        
        // Try to find Visual Studio installation
        std::vector<std::string> vsPaths = {
            "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\Common7\\IDE\\devenv.exe",
            "C:\\Program Files\\Microsoft Visual Studio\\2022\\Professional\\Common7\\IDE\\devenv.exe",
            "C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise\\Common7\\IDE\\devenv.exe",
            "C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\Community\\Common7\\IDE\\devenv.exe",
            "C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\Professional\\Common7\\IDE\\devenv.exe",
            "C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\Enterprise\\Common7\\IDE\\devenv.exe"
        };
        
        std::string vsExePath;
        for (const auto& path : vsPaths)
        {
            if (fs::exists(path))
            {
                vsExePath = path;
                break;
            }
        }
        
        if (vsExePath.empty())
        {
            AppendLog("[launch] Visual Studio not found in common locations");
            return false;
        }
        
        std::string cmd = "\"" + vsExePath + "\" \"" + slnPath + "\"";
        AppendLog("[launch] VS command: " + cmd);
        
        STARTUPINFOA si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOW;
        
        if (CreateProcessA(NULL, (LPSTR)cmd.c_str(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            AppendLog("[launch] VS launched successfully");
            return true;
        }
        else
        {
            DWORD error = GetLastError();
            AppendLog("[launch] VS launch failed, error: " + std::to_string(error));
            return false;
        }
    }

    static bool LaunchCursorWithFolder(const std::string& folderPath)
    {
        if (folderPath.empty()) return false;
        
        // Try to find Cursor installation
        std::vector<std::string> cursorPaths = {
            "C:\\Users\\" + GetEnvU8("USERNAME") + "\\AppData\\Local\\Programs\\cursor\\Cursor.exe",
            "C:\\Program Files\\Cursor\\Cursor.exe",
            "C:\\Program Files (x86)\\Cursor\\Cursor.exe"
        };
        
        std::string cursorExePath;
        for (const auto& path : cursorPaths)
        {
            if (fs::exists(path))
            {
                cursorExePath = path;
                break;
            }
        }
        
        if (cursorExePath.empty())
        {
            AppendLog("[launch] Cursor not found in common locations");
            return false;
        }
        
        std::string cmd = "\"" + cursorExePath + "\" \"" + folderPath + "\"";
        AppendLog("[launch] Cursor command: " + cmd);
        
        STARTUPINFOA si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOW;
        
        if (CreateProcessA(NULL, (LPSTR)cmd.c_str(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            AppendLog("[launch] Cursor launched successfully");
            return true;
        }
        else
        {
            DWORD error = GetLastError();
            AppendLog("[launch] Cursor launch failed, error: " + std::to_string(error));
            return false;
        }
    }

    static bool LaunchFeishu()
    {
        // Use saved path if available
        if (!g_feishuPath.empty())
        {
            std::string feishuExePath = g_feishuPath;
            if (fs::exists(feishuExePath))
            {
                std::string cmd = "\"" + feishuExePath + "\"";
                AppendLog("[launch] Feishu command: " + cmd);
                
                STARTUPINFOA si = { sizeof(si) };
                PROCESS_INFORMATION pi = {};
                si.dwFlags = STARTF_USESHOWWINDOW;
                si.wShowWindow = SW_SHOW;
                
                if (CreateProcessA(NULL, (LPSTR)cmd.c_str(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
                {
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                    AppendLog("[launch] Feishu launched successfully");
                    return true;
                }
                else
                {
                    DWORD error = GetLastError();
                    AppendLog("[launch] Feishu launch failed, error: " + std::to_string(error));
                    return false;
                }
            }
        }
        
        // Fallback: Try to find Feishu installation
        std::vector<std::string> feishuPaths = {
            "C:\\Users\\" + GetEnvU8("USERNAME") + "\\AppData\\Local\\Programs\\feishu\\feishu.exe",
            "C:\\Users\\" + GetEnvU8("USERNAME") + "\\AppData\\Local\\Programs\\lark\\lark.exe",
            "C:\\Program Files\\feishu\\feishu.exe",
            "C:\\Program Files\\lark\\lark.exe",
            "C:\\Program Files (x86)\\feishu\\feishu.exe",
            "C:\\Program Files (x86)\\lark\\lark.exe"
        };
        
        std::string feishuExePath;
        for (const auto& path : feishuPaths)
        {
            if (fs::exists(path))
            {
                feishuExePath = path;
                break;
            }
        }
        
        if (feishuExePath.empty())
        {
            AppendLog("[launch] Feishu not found in common locations");
            return false;
        }
        
        std::string cmd = "\"" + feishuExePath + "\"";
        AppendLog("[launch] Feishu command: " + cmd);
        
        STARTUPINFOA si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOW;
        
        if (CreateProcessA(NULL, (LPSTR)cmd.c_str(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            AppendLog("[launch] Feishu launched successfully");
            return true;
        }
        else
        {
            DWORD error = GetLastError();
            AppendLog("[launch] Feishu launch failed, error: " + std::to_string(error));
            return false;
        }
    }

    static bool LaunchWechat()
    {
        // Use saved path if available
        if (!g_wechatPath.empty())
        {
            std::string wechatExePath = g_wechatPath;
            if (fs::exists(wechatExePath))
            {
                std::string cmd = "\"" + wechatExePath + "\"";
                AppendLog("[launch] WeChat command: " + cmd);
                
                STARTUPINFOA si = { sizeof(si) };
                PROCESS_INFORMATION pi = {};
                si.dwFlags = STARTF_USESHOWWINDOW;
                si.wShowWindow = SW_SHOW;
                
                if (CreateProcessA(NULL, (LPSTR)cmd.c_str(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
                {
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                    AppendLog("[launch] WeChat launched successfully");
                    return true;
                }
                else
                {
                    DWORD error = GetLastError();
                    AppendLog("[launch] WeChat launch failed, error: " + std::to_string(error));
                    return false;
                }
            }
        }
        
        // Fallback: Try to find WeChat installation
        std::vector<std::string> wechatPaths = {
            "C:\\Program Files\\Tencent\\Weixin\\Weixin.exe",
            "C:\\Users\\" + GetEnvU8("USERNAME") + "\\AppData\\Local\\Tencent\\WeChat\\WeChat.exe",
            "C:\\Program Files\\Tencent\\WeChat\\WeChat.exe",
            "C:\\Program Files (x86)\\Tencent\\WeChat\\WeChat.exe"
        };
        
        std::string wechatExePath;
        for (const auto& path : wechatPaths)
        {
            if (fs::exists(path))
            {
                wechatExePath = path;
                break;
            }
        }
        
        if (wechatExePath.empty())
        {
            AppendLog("[launch] WeChat not found in common locations");
            return false;
        }
        
        std::string cmd = "\"" + wechatExePath + "\"";
        AppendLog("[launch] WeChat command: " + cmd);
        
        STARTUPINFOA si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOW;
        
        if (CreateProcessA(NULL, (LPSTR)cmd.c_str(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            AppendLog("[launch] WeChat launched successfully");
            return true;
        }
        else
        {
            DWORD error = GetLastError();
            AppendLog("[launch] WeChat launch failed, error: " + std::to_string(error));
            return false;
        }
    }

    static std::string WideToUtf8(const std::wstring& w)
    {
        if (w.empty()) return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

    static std::string UrlDecode(const std::string& s)
    {
        std::string out; out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == '%' && i + 2 < s.size())
            {
                auto hex = s.substr(i + 1, 2);
                char* endp = nullptr;
                int v = (int)strtol(hex.c_str(), &endp, 16);
                if (endp != nullptr && *endp == '\0') { out.push_back((char)v); i += 2; continue; }
            }
            if (s[i] == '+') out.push_back(' '); else out.push_back(s[i]);
        }
        return out;
    }

    // Forward declaration for use below
    static bool DecodeFileUriToWindowsPath(const std::string& uri, std::string& outPath);

    static bool ExtractLastFileUriWindowsPath(const std::string& text, std::string& outPath)
    {
        outPath.clear();
        size_t pos = text.rfind("file:///");
        if (pos == std::string::npos) return false;
        size_t start = text.rfind('"', pos);
        size_t end = text.find('"', pos);
        if (start == std::string::npos || end == std::string::npos || end <= start) return false;
        std::string uri = text.substr(start + 1, end - start - 1);
        return DecodeFileUriToWindowsPath(uri, outPath);
    }

    static bool DecodeFileUriToWindowsPath(const std::string& uri, std::string& outPath)
    {
        // Expect file:///D:/path or file:///D%3A/path
        const std::string prefix = "file:///";
        if (uri.rfind(prefix, 0) != 0) return false;
        std::string rest = uri.substr(prefix.size());
        rest = UrlDecode(rest);
        for (auto& ch : rest) { if (ch == '/') ch = '\\'; }
        outPath = rest;
        return true;
    }

    static std::string GetEnvU8(const char* name)
    {
        char* v = nullptr; size_t len = 0; _dupenv_s(&v, &len, name);
        std::string s = v ? std::string(v) : std::string();
        if (v) free(v);
        return s;
    }


    // Helpers to flatten COM interaction and reduce nesting
    static bool ParsePidFromRotName(const std::wstring& displayName, DWORD& pidOut)
    {
        pidOut = 0;
        // Preferred: !VisualStudio.DTE.x.y:PID
        size_t colon = displayName.rfind(L':');
        if (colon != std::wstring::npos && colon + 1 < displayName.size())
        {
            try {
                unsigned long v = std::stoul(displayName.substr(colon + 1));
                pidOut = (DWORD)v;
                return pidOut != 0;
            } catch (...) {}
        }
        // Fallback: last token after dot may be pid on some installs
        size_t dot = displayName.rfind(L'.');
        if (dot != std::wstring::npos && dot + 1 < displayName.size())
        {
            try {
                unsigned long v = std::stoul(displayName.substr(dot + 1));
                pidOut = (DWORD)v;
                return pidOut != 0;
            } catch (...) {}
        }
        return false;
    }
    static bool GetPidFromDTE(IDispatch* pDisp, DWORD& pidOut)
    {
        pidOut = 0;
        if (!pDisp) return false;
        DISPID dispidMainWindow = 0; OLECHAR* nameMainWindow = L"MainWindow";
        if (FAILED(pDisp->GetIDsOfNames(IID_NULL, &nameMainWindow, 1, LOCALE_USER_DEFAULT, &dispidMainWindow)))
        {
            AppendLog("[vs] GetIDsOfNames(MainWindow) failed");
            return false;
        }
        VARIANT resultMainWindow; VariantInit(&resultMainWindow);
        DISPPARAMS noArgs = {0};
        if (FAILED(pDisp->Invoke(dispidMainWindow, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &noArgs, &resultMainWindow, NULL, NULL)))
        {
            AppendLog("[vs] Invoke(MainWindow) failed");
            return false;
        }
        bool ok = false;
        if (resultMainWindow.vt == VT_DISPATCH && resultMainWindow.pdispVal)
        {
            IDispatch* pMainWin = resultMainWindow.pdispVal;
            DISPID dispidHWnd = 0; OLECHAR* nameHWnd = L"HWnd";
            if (SUCCEEDED(pMainWin->GetIDsOfNames(IID_NULL, &nameHWnd, 1, LOCALE_USER_DEFAULT, &dispidHWnd)))
            {
                VARIANT resultHwnd; VariantInit(&resultHwnd);
                auto readPidFromVariant = [&](const VARIANT& v)->bool {
                    INT_PTR hwndInt = 0;
                    switch (v.vt)
                    {
                        case VT_I2: hwndInt = (INT_PTR)v.iVal; break;
                        case VT_I4: hwndInt = (INT_PTR)v.lVal; break;
                        case VT_UI4: hwndInt = (INT_PTR)v.ulVal; break;
                        case VT_I8: hwndInt = (INT_PTR)v.llVal; break;
                        case VT_UI8: hwndInt = (INT_PTR)v.ullVal; break;
                        default:
                            AppendLog(std::string("[vs] HWnd VARIANT vt=") + std::to_string((int)v.vt));
                            break;
                    }
                    if (hwndInt)
                    {
                        GetWindowThreadProcessId((HWND)hwndInt, &pidOut);
                        if (pidOut != 0)
                        {
                            AppendLog(std::string("[vs] DTE hwnd=") + std::to_string((long long)hwndInt) + std::string(" pid=") + std::to_string((unsigned long)pidOut));
                            return true;
                        }
                    }
                    return false;
                };
                if (SUCCEEDED(pMainWin->Invoke(dispidHWnd, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &noArgs, &resultHwnd, NULL, NULL)))
                {
                    ok = readPidFromVariant(resultHwnd);
                    // short retry if first read fails
                    if (!ok)
                    {
                        Sleep(80);
                        VariantClear(&resultHwnd);
                        VariantInit(&resultHwnd);
                        if (SUCCEEDED(pMainWin->Invoke(dispidHWnd, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &noArgs, &resultHwnd, NULL, NULL)))
                        {
                            ok = readPidFromVariant(resultHwnd);
                        }
                    }
                }
                VariantClear(&resultHwnd);
            }
        }
        else
        {
            AppendLog("[vs] MainWindow not a dispatch");
        }
        VariantClear(&resultMainWindow);
        return ok;
    }

    static bool TryGetSolutionFullName(IDispatch* pDisp, std::string& slnOut)
    {
        slnOut.clear();
        if (!pDisp) return false;
        
        AppendLog("[vs] TryGetSolutionFullName: starting...");
        
        DISPID dispidSolution = 0; 
        OLECHAR* nameSolution = L"Solution";
        if (FAILED(pDisp->GetIDsOfNames(IID_NULL, &nameSolution, 1, LOCALE_USER_DEFAULT, &dispidSolution)))
        {
            AppendLog("[vs] GetIDsOfNames(Solution) failed");
            return false;
        }
        
        AppendLog("[vs] Got Solution DISPID: " + std::to_string(dispidSolution));
        
        VARIANT resultSolution; 
        VariantInit(&resultSolution); 
        DISPPARAMS noArgs = {0};
        HRESULT hrInvokeSolution = pDisp->Invoke(dispidSolution, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &noArgs, &resultSolution, NULL, NULL);
        if (FAILED(hrInvokeSolution))
        {
            AppendLog(std::string("[vs] Invoke(Solution) failed hr=") + std::to_string((long)hrInvokeSolution));
            VariantClear(&resultSolution);
            return false;
        }
        
        AppendLog("[vs] Successfully invoked Solution property, vt=" + std::to_string((int)resultSolution.vt));
        
        bool ok = false;
        if (resultSolution.vt == VT_DISPATCH && resultSolution.pdispVal)
        {
            AppendLog("[vs] Solution is a dispatch object");
            IDispatch* pSolution = resultSolution.pdispVal;
            DISPID dispidFullName = 0; 
            OLECHAR* nameFullName = L"FullName";
            HRESULT hrNameFN = pSolution->GetIDsOfNames(IID_NULL, &nameFullName, 1, LOCALE_USER_DEFAULT, &dispidFullName);
            if (SUCCEEDED(hrNameFN))
            {
                AppendLog("[vs] Got FullName DISPID: " + std::to_string(dispidFullName));
                VARIANT resultFullName; 
                VariantInit(&resultFullName);
                HRESULT hrInvokeFN = pSolution->Invoke(dispidFullName, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &noArgs, &resultFullName, NULL, NULL);
                if (SUCCEEDED(hrInvokeFN))
                {
                    AppendLog("[vs] Successfully invoked FullName property, vt=" + std::to_string((int)resultFullName.vt));
                    if (resultFullName.vt == VT_BSTR && resultFullName.bstrVal)
                    {
                        slnOut = WideToUtf8(resultFullName.bstrVal);
                        AppendLog(std::string("[vs] Solution.FullName=") + slnOut);
                        ok = !slnOut.empty();
                    }
                    else if (resultFullName.vt == VT_EMPTY || resultFullName.vt == VT_NULL)
                    {
                        AppendLog("[vs] Solution.FullName is empty or null - VS may be in Open Folder mode");
                        ok = false; // 空solution，但不算错误
                    }
                    else
                    {
                        AppendLog(std::string("[vs] Solution.FullName unexpected vt=") + std::to_string((int)resultFullName.vt));
                    }
                }
                else
                {
                    AppendLog(std::string("[vs] Invoke(Solution.FullName) failed hr=") + std::to_string((long)hrInvokeFN));
                }
                VariantClear(&resultFullName);
            }
            else
            {
                AppendLog(std::string("[vs] GetIDsOfNames(FullName) failed hr=") + std::to_string((long)hrNameFN));
            }
        }
        else if (resultSolution.vt == VT_EMPTY || resultSolution.vt == VT_NULL)
        {
            AppendLog("[vs] Solution is empty or null - VS may be in Open Folder mode");
            ok = false; // 空solution，但不算错误
        }
        else
        {
            AppendLog(std::string("[vs] Solution is not a dispatch object, vt=") + std::to_string((int)resultSolution.vt));
        }
        VariantClear(&resultSolution);
        return ok;
    }

    static void SearchSlnNearDocument(const std::string& docPath, std::string& slnOut)
    {
        slnOut.clear();
        std::error_code ec2; fs::path pdir = fs::path(docPath).parent_path(); fs::path startDir = pdir; int depth = 0;
        while (!pdir.empty() && depth < 12)
        {
            ec2.clear();
            for (fs::directory_iterator dit(pdir, fs::directory_options::skip_permission_denied, ec2), dend; dit != dend; dit.increment(ec2))
            {
                if (ec2) { ec2.clear(); continue; }
                const fs::directory_entry& e = *dit;
                if (e.is_regular_file(ec2) && !ec2)
                {
                    std::string ext = e.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)std::tolower(c); });
                    if (ext == ".sln")
                    {
                        slnOut = e.path().string();
                        AppendLog(std::string("[vs] Found nearby solution: ") + slnOut);
                        return;
                    }
                }
            }
            if (!slnOut.empty()) break;
            pdir = pdir.parent_path();
            depth++;
        }
        if (slnOut.empty()) AppendLog(std::string("[vs] No .sln found near ") + startDir.string());
    }











    // Step 2: 通过进程文件句柄枚举获取solution路径（需要管理员权限）
    static std::string TryGetSolutionFromProcessHandles(DWORD pid)
    {
        AppendLog("[vs] TryGetSolutionFromProcessHandles: pid=" + std::to_string((unsigned long)pid));
        
        // 需要管理员权限才能枚举其他进程的句柄
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (!hProcess) 
        {
            AppendLog("[vs] TryGetSolutionFromProcessHandles: OpenProcess failed - need admin privileges");
            return "";
        }
        
        // 使用NtQuerySystemInformation获取系统句柄信息
        typedef NTSTATUS (WINAPI *PNtQuerySystemInformation)(ULONG, PVOID, ULONG, PULONG);
        HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
        if (!hNtdll)
        {
            AppendLog("[vs] TryGetSolutionFromProcessHandles: Failed to get ntdll.dll");
            CloseHandle(hProcess);
            return "";
        }
        
        PNtQuerySystemInformation NtQuerySystemInformation = 
            (PNtQuerySystemInformation)GetProcAddress(hNtdll, "NtQuerySystemInformation");
        if (!NtQuerySystemInformation)
        {
            AppendLog("[vs] TryGetSolutionFromProcessHandles: Failed to get NtQuerySystemInformation");
            CloseHandle(hProcess);
            return "";
        }
        
        // 获取系统句柄信息
        ULONG bufferSize = 0x10000;
        PVOID buffer = VirtualAlloc(NULL, bufferSize, MEM_COMMIT, PAGE_READWRITE);
        if (!buffer)
        {
            AppendLog("[vs] TryGetSolutionFromProcessHandles: Failed to allocate buffer");
            CloseHandle(hProcess);
            return "";
        }
        
        NTSTATUS status = NtQuerySystemInformation(16, buffer, bufferSize, &bufferSize); // SystemHandleInformation
        if (status != 0)
        {
            AppendLog("[vs] TryGetSolutionFromProcessHandles: NtQuerySystemInformation failed with status " + std::to_string(status));
            VirtualFree(buffer, 0, MEM_RELEASE);
            CloseHandle(hProcess);
            return "";
        }
        
        // 解析句柄信息结构
        typedef struct _SYSTEM_HANDLE_TABLE_ENTRY_INFO {
            ULONG ProcessId;
            BYTE ObjectTypeNumber;
            BYTE Flags;
            USHORT Handle;
            PVOID Object;
            ULONG_PTR GrantedAccess;
        } SYSTEM_HANDLE_TABLE_ENTRY_INFO, *PSYSTEM_HANDLE_TABLE_ENTRY_INFO;
        
        typedef struct _SYSTEM_HANDLE_INFORMATION {
            ULONG NumberOfHandles;
            SYSTEM_HANDLE_TABLE_ENTRY_INFO Handles[1];
        } SYSTEM_HANDLE_INFORMATION, *PSYSTEM_HANDLE_INFORMATION;
        
        PSYSTEM_HANDLE_INFORMATION handleInfo = (PSYSTEM_HANDLE_INFORMATION)buffer;
        std::vector<std::string> candidateSolutions;
        
        for (ULONG i = 0; i < handleInfo->NumberOfHandles; i++)
        {
            SYSTEM_HANDLE_TABLE_ENTRY_INFO& handle = handleInfo->Handles[i];
            
            // 只检查目标进程的句柄
            if (handle.ProcessId != pid) continue;
            
            // 尝试获取句柄的文件名
            HANDLE hFile = (HANDLE)handle.Handle;
            wchar_t fileName[MAX_PATH];
            DWORD fileNameLen = GetFinalPathNameByHandleW(hFile, fileName, MAX_PATH, FILE_NAME_NORMALIZED);
            
            if (fileNameLen > 0 && fileNameLen < MAX_PATH)
            {
                std::string filePath = WideToUtf8(fileName);
                
                // 检查是否是.sln文件
                if (filePath.find(".sln") != std::string::npos && 
                    filePath.find("Dear-ImGui-Tool") == std::string::npos &&
                    std::filesystem::exists(filePath))
                {
                    AppendLog("[vs] TryGetSolutionFromProcessHandles: Found solution handle: " + filePath);
                    candidateSolutions.push_back(filePath);
                }
            }
        }
        
        VirtualFree(buffer, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        
        // 如果有多个候选，选择与活动文档目录最近的
        if (!candidateSolutions.empty())
        {
            if (candidateSolutions.size() == 1)
            {
                AppendLog("[vs] TryGetSolutionFromProcessHandles: Single solution found: " + candidateSolutions[0]);
                return candidateSolutions[0];
            }
            else
            {
                AppendLog("[vs] TryGetSolutionFromProcessHandles: Multiple solutions found, using first: " + candidateSolutions[0]);
                return candidateSolutions[0];
            }
        }
        
        AppendLog("[vs] TryGetSolutionFromProcessHandles: No solution handles found");
        return "";
    }
    
    // Step 3: 解析进程命令行，支持.slnf文件
    static std::string TryGetSolutionFromCommandLine(DWORD pid)
    {
        AppendLog("[vs] TryGetSolutionFromCommandLine: pid=" + std::to_string((unsigned long)pid));
        
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (!hProcess) 
        {
            AppendLog("[vs] TryGetSolutionFromCommandLine: OpenProcess failed");
            return "";
        }
        
        // 获取进程命令行
        wchar_t commandLine[4096];
        DWORD commandLineLen = 0;
        
        // 使用NtQueryInformationProcess获取命令行
        typedef NTSTATUS (WINAPI *PNtQueryInformationProcess)(HANDLE, ULONG, PVOID, ULONG, PULONG);
        HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
        if (hNtdll)
        {
            PNtQueryInformationProcess NtQueryInformationProcess = 
                (PNtQueryInformationProcess)GetProcAddress(hNtdll, "NtQueryInformationProcess");
            
            if (NtQueryInformationProcess)
            {
                typedef struct _PROCESS_BASIC_INFORMATION {
                    PVOID Reserved1;
                    PVOID PebBaseAddress;
                    PVOID Reserved2_0;
                    PVOID Reserved2_1;
                    PVOID UniqueProcessId;
                    PVOID Reserved3;
                } PROCESS_BASIC_INFORMATION;
                
                PROCESS_BASIC_INFORMATION pbi;
                NTSTATUS status = NtQueryInformationProcess(hProcess, 0, &pbi, sizeof(pbi), NULL);
                if (status == 0 && pbi.PebBaseAddress)
                {
                    // 读取PEB中的命令行信息
                    PVOID commandLinePtr = nullptr;
                    SIZE_T bytesRead;
                    if (ReadProcessMemory(hProcess, (PVOID)((BYTE*)pbi.PebBaseAddress + 0x70), &commandLinePtr, sizeof(commandLinePtr), &bytesRead))
                    {
                        if (commandLinePtr && ReadProcessMemory(hProcess, commandLinePtr, commandLine, sizeof(commandLine), &bytesRead))
                        {
                            commandLineLen = (DWORD)bytesRead / sizeof(wchar_t);
                        }
                    }
                }
            }
        }
        
        CloseHandle(hProcess);
        
        if (commandLineLen > 0)
        {
            std::wstring cmdLine(commandLine, commandLineLen);
            std::string cmdLineUtf8 = WideToUtf8(cmdLine);
            AppendLog("[vs] Process command line: " + cmdLineUtf8);
            
            // 查找.sln或.slnf文件路径
            size_t slnPos = cmdLineUtf8.find(".sln");
            size_t slnfPos = cmdLineUtf8.find(".slnf");
            
            if (slnfPos != std::string::npos)
            {
                // 处理.slnf文件
                size_t pathStart = cmdLineUtf8.find_last_of(" \t", slnfPos);
                if (pathStart == std::string::npos) pathStart = 0;
                else pathStart++;
                
                std::string slnfPath = cmdLineUtf8.substr(pathStart, slnfPos + 5 - pathStart);
                if (std::filesystem::exists(slnfPath))
                {
                    AppendLog("[vs] Found .slnf file: " + slnfPath);
                    
                    // 解析.slnf文件，查找solution路径
                    std::ifstream file(slnfPath);
                    if (file.is_open())
                    {
                        std::string line;
                        while (std::getline(file, line))
                        {
                            if (line.find("solution") != std::string::npos && line.find(".sln") != std::string::npos)
                            {
                                // 提取solution路径
                                size_t start = line.find("\"");
                                size_t end = line.find("\"", start + 1);
                                if (start != std::string::npos && end != std::string::npos)
                                {
                                    std::string slnPath = line.substr(start + 1, end - start - 1);
                                    if (std::filesystem::exists(slnPath))
                                    {
                                        AppendLog("[vs] Resolved .slnf to solution: " + slnPath);
                                        return slnPath;
                                    }
                                }
                            }
                        }
                        file.close();
                    }
                }
            }
            else if (slnPos != std::string::npos)
            {
                // 处理.sln文件
                size_t pathStart = cmdLineUtf8.find_last_of(" \t", slnPos);
                if (pathStart == std::string::npos) pathStart = 0;
                else pathStart++;
                
                std::string slnPath = cmdLineUtf8.substr(pathStart, slnPos + 4 - pathStart);
                if (std::filesystem::exists(slnPath))
                {
                    AppendLog("[vs] Found solution in command line: " + slnPath);
                    return slnPath;
                }
            }
        }
        
        return "";
    }
    
    // Step 4: 从活动文档路径向上搜索.sln文件（兜底方法）
    static std::string TryGetSolutionFromActiveDocument(const std::string& activeDocPath)
    {
        if (activeDocPath.empty()) return "";
        
        AppendLog("[vs] TryGetSolutionFromActiveDocument: searching from " + activeDocPath);
        
        std::filesystem::path docPath(activeDocPath);
        std::filesystem::path currentDir = docPath.parent_path();
        int depth = 0;
        const int MAX_DEPTH = 8;
        
        while (!currentDir.empty() && depth < MAX_DEPTH)
        {
            try
            {
                for (const auto& entry : std::filesystem::directory_iterator(currentDir))
                {
                    if (entry.is_regular_file() && entry.path().extension() == ".sln")
                    {
                        std::string slnPath = entry.path().string();
                        if (slnPath.find("Dear-ImGui-Tool") == std::string::npos)
                        {
                            AppendLog("[vs] Found solution near active document: " + slnPath);
                            return slnPath;
                        }
                    }
                }
            }
            catch (const std::exception& e)
            {
                AppendLog("[vs] TryGetSolutionFromActiveDocument: Exception: " + std::string(e.what()));
            }
            
            currentDir = currentDir.parent_path();
            depth++;
        }
        
        AppendLog("[vs] TryGetSolutionFromActiveDocument: No solution found");
        return "";
    }

    static void TryFillFromActiveDocument(IDispatch* pDisp, DWORD pid, std::vector<VSInstance>& found, std::string& slnOut)
    {
        slnOut.clear();
        DISPID dispidActiveDoc = 0; OLECHAR* nameActiveDoc = L"ActiveDocument";
        HRESULT hrNameAD = pDisp->GetIDsOfNames(IID_NULL, &nameActiveDoc, 1, LOCALE_USER_DEFAULT, &dispidActiveDoc);
        if (FAILED(hrNameAD)) { AppendLog(std::string("[vs] GetIDsOfNames(ActiveDocument) failed hr=") + std::to_string((long)hrNameAD)); return; }
        VARIANT resultActiveDoc; VariantInit(&resultActiveDoc); DISPPARAMS noArgs = {0};
        HRESULT hrInvokeAD = pDisp->Invoke(dispidActiveDoc, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &noArgs, &resultActiveDoc, NULL, NULL);
        if (FAILED(hrInvokeAD)) { AppendLog(std::string("[vs] Invoke(ActiveDocument) failed hr=") + std::to_string((long)hrInvokeAD)); return; }
        AppendLog("[vs] ActiveDocument fetched");
        if (resultActiveDoc.vt != VT_DISPATCH || !resultActiveDoc.pdispVal) { AppendLog("[vs] ActiveDocument is null"); VariantClear(&resultActiveDoc); return; }
        IDispatch* pDoc = resultActiveDoc.pdispVal;
        DISPID dispidDocFullName = 0; OLECHAR* nameDocFullName = L"FullName";
        HRESULT hrNameDocFN = pDoc->GetIDsOfNames(IID_NULL, &nameDocFullName, 1, LOCALE_USER_DEFAULT, &dispidDocFullName);
        if (FAILED(hrNameDocFN)) { AppendLog(std::string("[vs] GetIDsOfNames(ActiveDocument.FullName) failed hr=") + std::to_string((long)hrNameDocFN)); VariantClear(&resultActiveDoc); return; }
        VARIANT resultDocFN; VariantInit(&resultDocFN);
        if (SUCCEEDED(pDoc->Invoke(dispidDocFullName, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &noArgs, &resultDocFN, NULL, NULL)))
        {
            if (resultDocFN.vt == VT_BSTR && resultDocFN.bstrVal)
            {
                std::string docPath = WideToUtf8(resultDocFN.bstrVal);
                AppendLog(std::string("[vs] pid ") + std::to_string((unsigned long)pid) + std::string(" ActiveDocument: ") + docPath);
                for (auto& inst : found) { if (inst.pid == pid) { inst.activeDocumentPath = docPath; } }
                SearchSlnNearDocument(docPath, slnOut);
            }
            VariantClear(&resultDocFN);
        }
        else
        {
            AppendLog("[vs] Invoke(ActiveDocument.FullName) failed");
        }
        VariantClear(&resultActiveDoc);
    }

    static void ProcessDteMoniker(IRunningObjectTable* pRot, IMoniker* pMoniker, std::vector<VSInstance>& found, DWORD pidHint)
    {
        // 参考C#代码的实现方式
        IUnknown* pUnk = NULL; 
        HRESULT hrGetObj = pRot->GetObject(pMoniker, &pUnk);
        if (FAILED(hrGetObj) || !pUnk) 
        { 
            AppendLog(std::string("[vs] GetObject(moniker) failed hr=") + std::to_string((long)hrGetObj)); 
            return; 
        }
        
        // 直接尝试获取DTE接口，就像C#代码中的 (DTE)comObject
        IDispatch* pDisp = NULL; 
        HRESULT hrQI = pUnk->QueryInterface(IID_IDispatch, (void**)&pDisp);
        if (FAILED(hrQI) || !pDisp) 
        { 
            AppendLog(std::string("[vs] QueryInterface(IDispatch) failed hr=") + std::to_string((long)hrQI)); 
            pUnk->Release(); 
            return; 
        }

        // 获取进程ID
        DWORD pid = 0; 
        if (!GetPidFromDTE(pDisp, pid) || pid == 0) 
        {
            if (pidHint != 0) 
            {
                pid = pidHint;
                AppendLog(std::string("[vs] Using pidHint from ROT: ") + std::to_string((unsigned long)pid));
            } 
            else 
            {
                AppendLog("[vs] GetPidFromDTE failed and no pidHint"); 
                pDisp->Release(); 
                pUnk->Release(); 
                return;
            }
        }
        
        // 直接获取Solution.FullName，就像C#代码中的 dte.Solution.FullName
        std::string sln; 
        bool gotSln = TryGetSolutionFullName(pDisp, sln);
        AppendLog(std::string("[vs] TryGetSolutionFullName result=") + (gotSln?"true":"false") + std::string(" sln=") + (sln.empty()?"<empty>":sln));
        
        // 如果COM方法成功，直接使用结果
        if (!sln.empty())
        {
            for (auto& inst : found) 
            { 
                if (inst.pid == pid) 
                { 
                    inst.solutionPath = sln; 
                    AppendLog(std::string("[vs] pid ") + std::to_string((unsigned long)pid) + std::string(" Set solutionPath via COM: ") + sln); 
                } 
            }
        }
        else
        {
            // COM接口失败，尝试备用方法
            AppendLog("[vs] COM interface failed, trying alternative methods for pid=" + std::to_string((unsigned long)pid));
            
            // Step 2: 尝试进程文件句柄枚举
            std::string handlePath = TryGetSolutionFromProcessHandles(pid);
            if (!handlePath.empty())
            {
                sln = handlePath;
                AppendLog("[vs] Found solution via process handles: " + sln);
            }
            else
            {
                // Step 3: 尝试命令行解析
                std::string cmdLinePath = TryGetSolutionFromCommandLine(pid);
                if (!cmdLinePath.empty())
                {
                    sln = cmdLinePath;
                    AppendLog("[vs] Found solution via command line: " + sln);
                }
                else
                {
                    // Step 4: 从活动文档路径向上搜索
                    TryFillFromActiveDocument(pDisp, pid, found, sln);
                    if (!sln.empty())
                    {
                        std::string docPath = TryGetSolutionFromActiveDocument(sln);
                        if (!docPath.empty())
                        {
                            sln = docPath;
                            AppendLog("[vs] Found solution via active document search: " + sln);
                        }
                    }
                }
            }
            
            // 设置备用方法找到的结果
            if (!sln.empty())
            {
                for (auto& inst : found) 
                { 
                    if (inst.pid == pid) 
                    { 
                        inst.solutionPath = sln; 
                        AppendLog(std::string("[vs] pid ") + std::to_string((unsigned long)pid) + std::string(" Set solutionPath via backup method: ") + sln); 
                    } 
                }
            }
            else
            {
                AppendLog(std::string("[vs] pid ") + std::to_string((unsigned long)pid) + std::string(" no solution resolved - VS may be in Open Folder mode"));
            }
        }
        
        pDisp->Release(); 
        pUnk->Release();
    }

    void Refresh()
    {
        AppendLog("[vs] RefreshVSInstances: begin");
        
        // Update system resources
        float currentTime = ImGui::GetTime();
        if (currentTime - g_lastResourceUpdate >= g_resourceUpdateInterval) {
            UpdateSystemResources();
            g_lastResourceUpdate = currentTime;
        }
        
        std::vector<VSInstance> found;
        std::vector<CursorInstance> foundCursor;
        std::string foundFeishuPath;
        bool foundFeishuRunning = false;
        std::string foundWechatPath;
        bool foundWechatRunning = false;

        // Load persisted prefs once per refresh to show defaults
        if (g_selectedSlnPath.empty() && g_selectedCursorFolder.empty())
        {
            LoadPrefs();
        }

        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnap == INVALID_HANDLE_VALUE)
            return;

        PROCESSENTRY32 pe;
        pe.dwSize = sizeof(PROCESSENTRY32);
        if (Process32First(hSnap, &pe))
        {
            do
            {
                std::string exeU;
            #ifdef UNICODE
                exeU = WideToUtf8(pe.szExeFile);
            #else
                exeU = pe.szExeFile;
            #endif
                std::string exeLower = exeU;
                std::transform(exeLower.begin(), exeLower.end(), exeLower.begin(), [](unsigned char c){ return (char)std::tolower(c); });
                if (exeLower == std::string("devenv.exe"))
                {
                    VSInstance inst;
                    inst.pid = pe.th32ProcessID;

                    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pe.th32ProcessID);
                    if (hProc)
                    {
                        char buf[MAX_PATH];
                        DWORD sz = (DWORD)sizeof(buf);
                        if (QueryFullProcessImageNameA(hProc, 0, buf, &sz))
                            inst.exePath.assign(buf, sz);
                        CloseHandle(hProc);
                    }
                    AppendLog(std::string("[vs] found devenv.exe pid=") + std::to_string((unsigned long)inst.pid) + (inst.exePath.empty() ? std::string(" path=<unknown>") : std::string(" path=") + inst.exePath));
                    found.push_back(inst);
                }
                else if (exeLower == std::string("cursor.exe"))
                {
                    CursorInstance cinst;
                    cinst.pid = pe.th32ProcessID;
                    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pe.th32ProcessID);
                    if (hProc)
                    {
                        char buf[MAX_PATH];
                        DWORD sz = (DWORD)sizeof(buf);
                        if (QueryFullProcessImageNameA(hProc, 0, buf, &sz))
                            cinst.exePath.assign(buf, sz);
                        CloseHandle(hProc);
                    }
                    // Cursor进程检测日志已删除
                    foundCursor.push_back(cinst);
                }
                else if (DetectProcessAndGetPath(exeLower, {"feishu.exe", "lark.exe"}, pe.th32ProcessID, foundFeishuPath, foundFeishuRunning, "feishu"))
                {
                    // Processed by DetectProcessAndGetPath
                }
                else if (exeLower == "weixin.exe" || exeLower == "wechat.exe")
                {
                    // 优先检测微信主程序
                    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pe.th32ProcessID);
                    if (hProc)
                    {
                        char buf[MAX_PATH];
                        DWORD sz = (DWORD)sizeof(buf);
                        if (QueryFullProcessImageNameA(hProc, 0, buf, &sz))
                            foundWechatPath.assign(buf, sz);
                        CloseHandle(hProc);
                    }
                    foundWechatRunning = true;
                    // 微信进程检测日志已删除
                }
                else if (exeLower == "wechatappex.exe")
                {
                    // 检测到微信插件进程，但不保存路径（只用于状态检测）
                    if (!foundWechatRunning)
                    {
                        foundWechatRunning = true;
                        // 微信插件进程检测日志已删除
                    }
                }
            } while (Process32Next(hSnap, &pe));
        }
        CloseHandle(hSnap);

        std::unordered_map<DWORD, std::string> pidToTitle;
        EnumWindows([](HWND hWnd, LPARAM lParam)->BOOL{
            if (!IsWindowVisible(hWnd)) return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(hWnd, &pid);
            if (pid == 0) return TRUE;
            wchar_t wtitle[512];
            int lenW = GetWindowTextW(hWnd, wtitle, (int)(sizeof(wtitle)/sizeof(wtitle[0])));
            if (lenW > 0)
            {
                std::wstring wstr(wtitle, lenW);
                std::string utf8 = WideToUtf8(wstr);
                auto* mapPtr = reinterpret_cast<std::unordered_map<DWORD, std::string>*>(lParam);
                auto it = mapPtr->find(pid);
                if (it == mapPtr->end() || (int)it->second.size() < (int)utf8.size())
                    (*mapPtr)[pid] = utf8;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&pidToTitle));

        AppendLog(std::string("[vs] Found ") + std::to_string(found.size()) + " VS processes before ROT processing");
        for (auto& inst : found)
        {
            auto it = pidToTitle.find(inst.pid);
            if (it != pidToTitle.end()) inst.windowTitle = it->second;
            AppendLog(std::string("[vs] VS process: pid=") + std::to_string((unsigned long)inst.pid) + 
                     std::string(" title=") + (inst.windowTitle.empty()?"<none>":inst.windowTitle) +
                     std::string(" solutionPath=") + (inst.solutionPath.empty()?"<none>":inst.solutionPath));
        }



        // 首先获取所有openedWindows中的文件夹
        std::vector<std::string> openedFolders;
        std::string appdata = GetEnvU8("APPDATA");
        // Cursor APPDATA日志已删除
        if (!appdata.empty())
        {
            fs::path p = fs::path(appdata) / "Cursor" / "User" / "globalStorage" / "storage.json";
            // Cursor storage.json检查日志已删除
            std::error_code ec; 
            if (fs::exists(p, ec))
            {
                // Cursor storage.json读取日志已删除
                std::ifstream ifs(p.string(), std::ios::binary);
                if (ifs)
                {
                    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                    // Cursor storage.json内容大小日志已删除
                    size_t openedWindowsPos = content.find("\"openedWindows\"");
                    if (openedWindowsPos != std::string::npos)
                    {
                        // Cursor openedWindows位置日志已删除
                        size_t pos = openedWindowsPos;
                        int folderCount = 0;
                        while (true)
                        {
                            size_t folderPos = content.find("\"folder\"", pos);
                            if (folderPos == std::string::npos) 
                            {
                                // Cursor文件夹条目统计日志已删除
                                break;
                            }
                            
                            folderCount++;
                            // Cursor文件夹条目位置日志已删除
                            
                            // 改进的JSON解析逻辑
                            // 查找 "folder": 后面的值
                            size_t colonPos = content.find(':', folderPos + 8);
                            if (colonPos == std::string::npos) break;
                            
                            // 跳过冒号后的空白字符
                            size_t valueStart = colonPos + 1;
                            while (valueStart < content.size() && (content[valueStart] == ' ' || content[valueStart] == '\t' || content[valueStart] == '\n' || content[valueStart] == '\r'))
                                valueStart++;
                            
                            if (valueStart >= content.size()) break;
                            
                            // 检查是否是字符串值（以双引号开始）
                            if (content[valueStart] != '"') break;
                            
                            // 找到字符串的结束位置
                            size_t valueEnd = valueStart + 1;
                            while (valueEnd < content.size() && content[valueEnd] != '"')
                            {
                                if (content[valueEnd] == '\\' && valueEnd + 1 < content.size())
                                    valueEnd += 2; // 跳过转义字符
                                else
                                    valueEnd++;
                            }
                            
                            if (valueEnd >= content.size()) break;
                            
                            std::string folderUri = content.substr(valueStart + 1, valueEnd - valueStart - 1);
                            // Cursor文件夹URI日志已删除
                            
                            std::string winPath;
                            if (DecodeFileUriToWindowsPath(folderUri, winPath))
                            {
                                openedFolders.push_back(winPath);
                                // Cursor解码文件夹路径日志已删除
                            }
                            else
                            {
                                // Cursor URI解码失败日志已删除
                            }
                            pos = valueEnd + 1;
                        }
                    }
                    else
                    {
                        // Cursor openedWindows未找到日志已删除
                    }
                }
                else
                {
                    // Cursor storage.json读取失败日志已删除
                }
            }
            else
            {
                // Cursor storage.json不存在日志已删除
            }
        }
        else
        {
            // Cursor APPDATA环境变量为空日志已删除
        }
        // Cursor统计信息日志已删除

        // 如果没有从 openedWindows 解析到任何文件夹，则尝试从 lastActiveWindow 读取一个文件夹作为回退
        if (openedFolders.empty())
        {
            // Cursor回退机制日志已删除
            std::string appdata2 = GetEnvU8("APPDATA");
            if (!appdata2.empty())
            {
                fs::path p2 = fs::path(appdata2) / "Cursor" / "User" / "globalStorage" / "storage.json";
                std::error_code ec2;
                if (fs::exists(p2, ec2))
                {
                    std::ifstream ifs2(p2.string(), std::ios::binary);
                    if (ifs2)
                    {
                        std::string content2((std::istreambuf_iterator<char>(ifs2)), std::istreambuf_iterator<char>());
                        size_t lastActiveWindowPos = content2.find("\"lastActiveWindow\"");
                        // Cursor lastActiveWindow位置日志已删除
                        if (lastActiveWindowPos != std::string::npos)
                        {
                            size_t folderKeyPos = content2.find("\"folder\"", lastActiveWindowPos);
                            if (folderKeyPos != std::string::npos)
                            {
                                size_t colonPos = content2.find(':', folderKeyPos + 8);
                                if (colonPos != std::string::npos)
                                {
                                    size_t valueStart = colonPos + 1;
                                    while (valueStart < content2.size() && (content2[valueStart] == ' ' || content2[valueStart] == '\t' || content2[valueStart] == '\n' || content2[valueStart] == '\r')) valueStart++;
                                    if (valueStart < content2.size() && content2[valueStart] == '"')
                                    {
                                        size_t valueEnd = valueStart + 1;
                                        while (valueEnd < content2.size() && content2[valueEnd] != '"')
                                        {
                                            if (content2[valueEnd] == '\\' && valueEnd + 1 < content2.size()) valueEnd += 2; else valueEnd++;
                                        }
                                        if (valueEnd < content2.size())
                                        {
                                            std::string folderUri = content2.substr(valueStart + 1, valueEnd - valueStart - 1);
                                            std::string winPath2;
                                            if (DecodeFileUriToWindowsPath(folderUri, winPath2))
                                            {
                                                openedFolders.push_back(winPath2);
                                                // Cursor回退文件夹路径日志已删除
                                            }
                                            else
                                            {
                                                // Cursor回退URI解码失败日志已删除
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // 重置静态索引，确保每次刷新都从头开始分配
        static size_t cursorIndex = 0;
        cursorIndex = 0;  // 重置为0

        // 直接为每个Cursor实例分配openedFolders中的文件夹
        for (size_t idx = 0; idx < foundCursor.size(); ++idx)
        {
            auto& cinst = foundCursor[idx];
            auto it = pidToTitle.find(cinst.pid);
            if (it != pidToTitle.end()) cinst.windowTitle = it->second;
            
            // 直接分配openedFolders中的文件夹，按索引顺序
            if (idx < openedFolders.size())
            {
                cinst.folderPath = openedFolders[idx];
                fs::path folderPath = cinst.folderPath;
                cinst.workspaceName = folderPath.filename().string();
            }
            else
            {
            }
        }

        HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        bool didCoInit = SUCCEEDED(hr);
        AppendLog(std::string("[vs] CoInitializeEx hr=") + std::to_string((long)hr) + std::string(" didCoInit=") + (didCoInit?"true":"false"));
        
        // 检查当前进程权限
        HANDLE hToken;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
        {
            TOKEN_ELEVATION elevation;
            DWORD size = sizeof(TOKEN_ELEVATION);
            if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &size))
            {
                AppendLog(std::string("[vs] Current process elevation: ") + (elevation.TokenIsElevated ? "Elevated" : "Not Elevated"));
            }
            CloseHandle(hToken);
        }
        
        static bool comSecurityInitialized = false;
        if (didCoInit && !comSecurityInitialized)
        {
            // 设置更宽松的COM安全级别以访问ROT
            HRESULT hrSec = CoInitializeSecurity(NULL, -1, NULL, NULL,
                RPC_C_AUTHN_LEVEL_NONE, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
            AppendLog(std::string("[vs] CoInitializeSecurity hr=") + std::to_string((long)hrSec));
            if (SUCCEEDED(hrSec)) 
            {
                comSecurityInitialized = true;
                AppendLog("[vs] COM security initialized with RPC_C_AUTHN_LEVEL_NONE");
            }
            else
            {
                // 如果失败，尝试更宽松的设置
                hrSec = CoInitializeSecurity(NULL, -1, NULL, NULL,
                    RPC_C_AUTHN_LEVEL_CONNECT, RPC_C_IMP_LEVEL_IDENTIFY, NULL, EOAC_NONE, NULL);
                AppendLog(std::string("[vs] CoInitializeSecurity retry hr=") + std::to_string((long)hrSec));
                if (SUCCEEDED(hrSec)) 
                {
                    comSecurityInitialized = true;
                    AppendLog("[vs] COM security initialized with fallback settings");
                }
            }
        }
        IRunningObjectTable* pRot = NULL;
        IEnumMoniker* pEnum = NULL;
        HRESULT hrRot = GetRunningObjectTable(0, &pRot);
        AppendLog(std::string("[vs] GetRunningObjectTable hr=") + std::to_string((long)hrRot));
        if (SUCCEEDED(hrRot) && pRot)
        {
            // 获取枚举器
            HRESULT hrEnum = pRot->EnumRunning(&pEnum);
            AppendLog(std::string("[vs] EnumRunning hr=") + std::to_string((long)hrEnum));
            
            if (SUCCEEDED(hrEnum) && pEnum)
            {
                ULONG fetched = 0;
                int count = 0;
                CComPtr<IMoniker> spMoniker;
        
                                 // 枚举所有 moniker，处理DTE对象
                 while (pEnum->Next(1, &spMoniker, &fetched) == S_OK)
                 {
                     CComPtr<IBindCtx> spCtx;
                     CreateBindCtx(0, &spCtx);
         
                     LPOLESTR displayName = nullptr;
                     if (SUCCEEDED(spMoniker->GetDisplayName(spCtx, nullptr, &displayName)))
                     {
                         std::wstring ws(displayName);
                         std::string s(ws.begin(), ws.end());
                         AppendLog("[vs] ROT entry: " + s);
                         
                         // 检查是否是Visual Studio DTE对象
                         if (s.find("!VisualStudio.DTE") != std::string::npos)
                         {
                             AppendLog("[vs] Found Visual Studio DTE object: " + s);
                             
                             // 解析PID
                             DWORD pidHint = 0;
                             ParsePidFromRotName(ws, pidHint);
                             AppendLog("[vs] Parsed PID hint: " + std::to_string((unsigned long)pidHint));
                             
                             // 处理DTE moniker
                             ProcessDteMoniker(pRot, spMoniker, found, pidHint);
                         }
                         
                         CoTaskMemFree(displayName);
                     }
         
                     spMoniker.Release();
                     count++;
                 }
        
                AppendLog("[vs] Total ROT entries enumerated: " + std::to_string(count));
            }
            else
            {
                AppendLog("[vs] EnumRunning failed");
            }
        }
        else
        {
            AppendLog("[vs] GetRunningObjectTable failed");
        }
        
        // 正确释放 COM 对象
        if (pEnum)
        {
            pEnum->Release();
            pEnum = NULL;
        }
        if (pRot)
        {
            pRot->Release();
            pRot = NULL;
        }
        
        // 只使用COM接口获取solution路径，不使用备用方法
        AppendLog("[vs] Solution detection completed - only COM interface used");

        {
            std::lock_guard<std::mutex> lock(g_vsMutexVS);
            g_vsList.swap(found);
            g_cursorList.swap(foundCursor);
            // 只有在检测到飞书运行时才更新路径，避免覆盖已保存的路径
            if (foundFeishuRunning)
            {
                g_feishuPath = foundFeishuPath;
            }
            g_feishuRunning = foundFeishuRunning;
            // 微信路径不自动更新，只有用户手动勾选时才保存
            // if (foundWechatRunning)
            // {
            //     g_wechatPath = foundWechatPath;
            // }
            g_wechatRunning = foundWechatRunning;
            g_currentWechatPath = foundWechatPath;  // 更新当前检测到的路径
        }
        // VS实例刷新结束日志已删除
        // Cursor实例刷新结束日志已删除
        // 飞书状态日志已删除
        // 微信状态日志已删除
        for (const auto& inst : g_vsList)
        {
            // VS实例汇总日志已删除
        }
    }

    void DrawVSUI()
    {
        // Ensure preferences are loaded on first UI draw
        EnsurePrefsLoaded();
        
        // 启动动画逻辑
        float currentTime = ImGui::GetTime();
        if (g_showStartupAnimation)
        {
            if (g_startupAnimationTime == 0.0f)
            {
                g_startupAnimationTime = currentTime;
            }
            
            float elapsed = currentTime - g_startupAnimationTime;
            float progress = elapsed / g_startupAnimationDuration;
            
            // 根据进度更新动画步骤
            if (progress < 0.25f) g_startupAnimationStep = 0;
            else if (progress < 0.5f) g_startupAnimationStep = 1;
            else if (progress < 0.75f) g_startupAnimationStep = 2;
            else g_startupAnimationStep = 3;
            
            // 动画完成后隐藏
            if (progress >= 1.0f)
            {
                g_showStartupAnimation = false;
                // 动画完成后立即执行一次刷新
                Refresh();
                g_lastRefreshTime = currentTime;
            }
        }
        
        // 自动刷新逻辑
        if (g_autoRefreshEnabled && (currentTime - g_lastRefreshTime) >= g_autoRefreshInterval)
        {
            Refresh();
            g_lastRefreshTime = currentTime;
        }
        
        // 设置窗口为可调整大小，并设置最小尺寸
        ImGui::SetNextWindowSizeConstraints(ImVec2(800, 600), ImVec2(FLT_MAX, FLT_MAX));
        ImGui::Begin(" VS & Cursor & Feishu Manager 🚀", nullptr, ImGuiWindowFlags_None);
        
        // 启动动画显示
        if (g_showStartupAnimation)
        {
            float currentTime = ImGui::GetTime();
            float elapsed = currentTime - g_startupAnimationTime;
            float progress = elapsed / g_startupAnimationDuration;
            
            // 更新科技感动画时间
            g_particleTime += ImGui::GetIO().DeltaTime * 3.0f;
            g_dataStreamTime += ImGui::GetIO().DeltaTime * 2.0f;
            g_glitchTime += ImGui::GetIO().DeltaTime * 1.5f;
            g_scanLineY += ImGui::GetIO().DeltaTime * 100.0f;
            
            // 获取窗口尺寸
            ImVec2 windowSize = ImGui::GetWindowSize();
            ImVec2 windowPos = ImGui::GetWindowPos();
            
            // 绘制背景网格效果
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            ImVec2 canvasPos = ImGui::GetCursorScreenPos();
            
            // 绘制扫描线效果
            float scanLineY = fmodf(g_scanLineY, windowSize.y);
            drawList->AddLine(
                ImVec2(canvasPos.x, canvasPos.y + scanLineY),
                ImVec2(canvasPos.x + windowSize.x, canvasPos.y + scanLineY),
                IM_COL32(0, 255, 0, 50),
                2.0f
            );
            
            // 绘制粒子效果
            for (int i = 0; i < 20; i++)
            {
                float x = fmodf(g_particleTime * 50.0f + i * 37.0f, windowSize.x);
                float y = fmodf(g_particleTime * 30.0f + i * 23.0f, windowSize.y);
                float alpha = 0.3f + 0.4f * sinf(g_particleTime + i);
                drawList->AddCircleFilled(
                    ImVec2(canvasPos.x + x, canvasPos.y + y),
                    2.0f,
                    IM_COL32(0, 255, 0, (int)(alpha * 255))
                );
            }
            
            // 绘制数据流动画
            for (int i = 0; i < 5; i++)
            {
                float x = fmodf(g_dataStreamTime * 100.0f + i * 200.0f, windowSize.x);
                float y = fmodf(g_dataStreamTime * 50.0f + i * 100.0f, windowSize.y);
                drawList->AddText(
                    ImVec2(canvasPos.x + x, canvasPos.y + y),
                    IM_COL32(0, 255, 0, 100),
                    "01"
                );
            }
            
            // 居中显示启动动画
            ImGui::SetCursorPosY(ImGui::GetWindowHeight() * 0.4f);
            
            // 添加故障效果
            if (g_glitchTime > 0.5f && g_glitchCounter < 3)
            {
                g_glitchTime = 0.0f;
                g_glitchCounter++;
                // 随机偏移文本位置
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (rand() % 10 - 5));
            }
            
            // 显示动画文本（带科技感样式）
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.5f, 1.0f));
            ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]); // 使用默认字体
            ImGui::TextWrapped("%s", g_startupAnimationTexts[g_startupAnimationStep]);
            ImGui::PopFont();
            ImGui::PopStyleColor();
            
            ImGui::Spacing();
            
            // 显示科技感进度条
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.0f, 1.0f, 0.5f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.05f, 0.05f, 0.05f, 1.0f));
            ImGui::ProgressBar(progress, ImVec2(-1, 25), "");
            ImGui::PopStyleColor();
            ImGui::PopStyleColor();
            
            // 添加进度条发光效果
            float glowAlpha = 0.3f + 0.2f * sinf(g_particleTime * 2.0f);
            drawList->AddRect(
                ImVec2(canvasPos.x + 10, canvasPos.y + ImGui::GetWindowHeight() * 0.4f + 60),
                ImVec2(canvasPos.x + windowSize.x - 10, canvasPos.y + ImGui::GetWindowHeight() * 0.4f + 85),
                IM_COL32(0, 255, 0, (int)(glowAlpha * 255)),
                5.0f,
                0,
                2.0f
            );
            
            ImGui::Spacing();
            
            // 显示进度百分比（带科技感）
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f), "[SYSTEM] %.0f%% COMPLETE", progress * 100.0f);
            
            // 添加系统状态信息
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "STATUS: INITIALIZING");
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "MEMORY: OK");
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "NETWORK: STABLE");
            
            // 添加一些装饰性的动画效果
            static float pulseTime = 0.0f;
            pulseTime += ImGui::GetIO().DeltaTime * 3.0f;
            float pulseAlpha = 0.5f + 0.4f * sinf(pulseTime);
            
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 20);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.5f, pulseAlpha));
            ImGui::Text("SYSTEM READY IN %.1f SECONDS", (1.0f - progress) * g_startupAnimationDuration);
            ImGui::PopStyleColor();
            
            ImGui::End(); // 正确结束窗口
            return; // 动画期间不显示其他内容
        }
        
        // 主界面科技感背景动画
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImVec2 windowSize = ImGui::GetWindowSize();
        
        // 更新动画时间
        static float mainUIParticleTime = 0.0f;
        mainUIParticleTime += ImGui::GetIO().DeltaTime * 2.0f;
        
        // 绘制角落装饰
        float cornerSize = 20.0f;
        static float cornerAnimationTime = 0.0f;
        cornerAnimationTime += ImGui::GetIO().DeltaTime * 2.0f;
        float cornerAlpha = 0.3f + 0.2f * sinf(cornerAnimationTime);
        
        // 左上角
        drawList->AddLine(ImVec2(canvasPos.x, canvasPos.y), ImVec2(canvasPos.x + cornerSize, canvasPos.y), 
                         IM_COL32(0, 255, 0, (int)(cornerAlpha * 255)), 2.0f);
        drawList->AddLine(ImVec2(canvasPos.x, canvasPos.y), ImVec2(canvasPos.x, canvasPos.y + cornerSize), 
                         IM_COL32(0, 255, 0, (int)(cornerAlpha * 255)), 2.0f);
        
        // 右上角
        drawList->AddLine(ImVec2(canvasPos.x + windowSize.x - cornerSize, canvasPos.y), ImVec2(canvasPos.x + windowSize.x, canvasPos.y), 
                         IM_COL32(0, 255, 0, (int)(cornerAlpha * 255)), 2.0f);
        drawList->AddLine(ImVec2(canvasPos.x + windowSize.x, canvasPos.y), ImVec2(canvasPos.x + windowSize.x, canvasPos.y + cornerSize), 
                         IM_COL32(0, 255, 0, (int)(cornerAlpha * 255)), 2.0f);
        
        // 左下角
        drawList->AddLine(ImVec2(canvasPos.x, canvasPos.y + windowSize.y - cornerSize), ImVec2(canvasPos.x, canvasPos.y + windowSize.y), 
                         IM_COL32(0, 255, 0, (int)(cornerAlpha * 255)), 2.0f);
        drawList->AddLine(ImVec2(canvasPos.x, canvasPos.y + windowSize.y), ImVec2(canvasPos.x + cornerSize, canvasPos.y + windowSize.y), 
                         IM_COL32(0, 255, 0, (int)(cornerAlpha * 255)), 2.0f);
        
        // 右下角
        drawList->AddLine(ImVec2(canvasPos.x + windowSize.x - cornerSize, canvasPos.y + windowSize.y), ImVec2(canvasPos.x + windowSize.x, canvasPos.y + windowSize.y), 
                         IM_COL32(0, 255, 0, (int)(cornerAlpha * 255)), 2.0f);
        drawList->AddLine(ImVec2(canvasPos.x + windowSize.x, canvasPos.y + windowSize.y - cornerSize), ImVec2(canvasPos.x + windowSize.x, canvasPos.y + windowSize.y), 
                         IM_COL32(0, 255, 0, (int)(cornerAlpha * 255)), 2.0f);
        
        // 顶部状态栏
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
        ImGui::BeginChild("TopStatusBar", ImVec2(0, 65), true);
        
        // 左侧：系统状态
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f), "SYSTEM:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "●");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f), "ONLINE");
        
        // 中间：时间显示
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() * 0.3f);
        time_t now = time(nullptr);
        struct tm* timeinfo = localtime(&now);
        char timeStr[64];
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", timeinfo);
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "TIME: %s", timeStr);
        
        // 右侧：应用统计
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() * 0.7f);
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "VS: %d | Cursor: %d | Configs: %d", 
                          (int)g_vsList.size(), (int)g_cursorList.size(), (int)g_savedConfigs.size());
        
        ImGui::EndChild();
        ImGui::PopStyleColor();
        
        // Header with refresh controls
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f), "🚀 VS & Cursor & Feishu Manager v2.0 😊");
        ImGui::SameLine();
        if (ImGui::Button("[Refresh]"))
        {
            ReplaceTool::AppendLog("[vs] UI: Refresh clicked");
            Refresh();
            g_lastRefreshTime = currentTime; // 更新最后刷新时间
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Auto Refresh", &g_autoRefreshEnabled))
        {
            if (g_autoRefreshEnabled)
            {
                AppendLog("[vs] Auto refresh enabled");
            }
            else
            {
                AppendLog("[vs] Auto refresh disabled");
            }
        }

        
        ImGui::Separator();
        
        // 获取当前窗口宽度，动态调整布局
        float windowWidth = ImGui::GetWindowWidth();
        bool useWideLayout = windowWidth > 1200;
        
        if (useWideLayout)
        {
            // 宽屏布局：三列，优化列宽比例
            ImGui::Columns(3, "MainContent", true);
            ImGui::SetColumnWidth(0, windowWidth * 0.35f);  // 系统监控列稍宽
            ImGui::SetColumnWidth(1, windowWidth * 0.35f);  // 控制中心列
            ImGui::SetColumnWidth(2, windowWidth * 0.30f);  // 数据管理列稍窄
        }
        else
        {
            // 窄屏布局：两列
            ImGui::Columns(2, "MainContent", true);
            ImGui::SetColumnWidth(0, windowWidth * 0.5f);
            ImGui::SetColumnWidth(1, windowWidth * 0.5f);
        }
        
        // Left column: Running Instances
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.5f, 1.0f));
        ImGui::Text("🔍 [SYSTEM MONITOR]");
        ImGui::PopStyleColor();
        
        // 添加系统状态指示器
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 20);
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "●");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "ONLINE");
        
        ImGui::Separator();
        
        // 移除左侧的系统资源监控，将移到下方
        
        std::vector<VSInstance> local;
        std::vector<CursorInstance> localCursor;
        std::string localFeishuPath;
        bool localFeishuRunning;
        std::string localWechatPath;
        bool localWechatRunning;
        std::string localCurrentWechatPath;
        {
            std::lock_guard<std::mutex> lock(g_vsMutexVS);
            local = g_vsList;
            localCursor = g_cursorList;
            localFeishuPath = g_feishuPath;
            localFeishuRunning = g_feishuRunning;
            localWechatPath = g_wechatPath;
            localWechatRunning = g_wechatRunning;
            localCurrentWechatPath = g_currentWechatPath;
        }
        
        // VS Instances
        if (!local.empty())
        {
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Visual Studio (%d)", (int)local.size());
            for (const auto& inst : local)
            {
                ImGui::BeginGroup();
                ImGui::Text("[PID] %lu", (unsigned long)inst.pid);
                if (!inst.windowTitle.empty())
                {
                    ImGui::TextWrapped("[Title] %s", inst.windowTitle.c_str());
                }
                if (!inst.solutionPath.empty())
                {
                    ImGui::TextWrapped("[Path] %s", inst.solutionPath.c_str());
                }
                else
                {
                    ImGui::TextWrapped("[Path] <no solution detected>");
                }
                
                // 总是显示选择框，即使没有solutionPath
                std::string pathKey = inst.solutionPath.empty() ? std::string("pid_") + std::to_string((unsigned long)inst.pid) : inst.solutionPath;
                bool checked = (g_selectedSlnPaths.find(pathKey) != g_selectedSlnPaths.end());
                std::string chkId = std::string("[Use this solution]##") + std::to_string((unsigned long)inst.pid);
                if (ImGui::Checkbox(chkId.c_str(), &checked))
                {
                    if (checked)
                    {
                        g_selectedSlnPaths.insert(pathKey);
                        // 保持向后兼容，设置第一个选中的作为主要选择
                        if (g_selectedSlnPath.empty())
                            g_selectedSlnPath = inst.solutionPath.empty() ? pathKey : inst.solutionPath;
                    }
                    else
                    {
                        g_selectedSlnPaths.erase(pathKey);
                        // 如果删除的是主要选择，选择下一个
                        std::string mainKey = g_selectedSlnPath;
                        if (mainKey == pathKey)
                        {
                            g_selectedSlnPath = g_selectedSlnPaths.empty() ? std::string() : *g_selectedSlnPaths.begin();
                        }
                    }
                }
                ImGui::EndGroup();
                ImGui::Spacing();
            }
        }
        else
        {
            ImGui::TextDisabled("No Visual Studio instances found");
        }
        
        ImGui::Spacing();
        
        // Cursor Instances (beautified table)
        if (!localCursor.empty())
        {
            int validCursorCount = 0;
            for (const auto& c : localCursor) { if (!c.folderPath.empty()) ++validCursorCount; }
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Cursor (%d)", validCursorCount);
            ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoHostExtendX;
            if (ImGui::BeginTable("cursor_table", 4, flags))
            {
                ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_WidthStretch, 2.0f);
                ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch, 2.5f);
                ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                ImGui::TableHeadersRow();

                for (const auto& c : localCursor)
                {
                    if (c.folderPath.empty()) continue;
                    ImGui::TableNextRow();

                    // PID
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%lu", (unsigned long)c.pid);

                    // Title
                    ImGui::TableSetColumnIndex(1);
                    if (!c.windowTitle.empty()) ImGui::TextWrapped("%s", c.windowTitle.c_str()); else ImGui::TextDisabled("<none>");

                    // Path
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextWrapped("%s", c.folderPath.c_str());

                    // Action
                    ImGui::TableSetColumnIndex(3);
                    bool isSelected = (g_selectedCursorFolders.find(c.folderPath) != g_selectedCursorFolders.end());
                    const char* label = isSelected ? "[Deselect]" : "[Select]";
                    if (ImGui::SmallButton((std::string(label) + "##" + std::to_string((unsigned long)c.pid)).c_str()))
                    {
                        if (isSelected)
                            g_selectedCursorFolders.erase(c.folderPath);
                        else
                            g_selectedCursorFolders.insert(c.folderPath);
                        // Keep backward-compatible single selection too
                        if (isSelected && g_selectedCursorFolder == c.folderPath)
                            g_selectedCursorFolder.clear();
                        else if (!isSelected)
                            g_selectedCursorFolder = c.folderPath;
                    }
                }

                ImGui::EndTable();
            }
        }
        else
        {
            ImGui::TextDisabled("No Cursor instances found");
        }
        
        ImGui::Spacing();
        
        // Feishu Status
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Feishu Status");
        ImGui::BeginGroup();
        if (localFeishuRunning)
        {
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "✓ Running");
        }
        else
        {
            ImGui::TextColored(ImVec4(0.8f, 0.2f, 0.2f, 1.0f), "✗ Not Running");
        }
        
        if (!localFeishuPath.empty())
        {
            ImGui::TextWrapped("[Path] %s", localFeishuPath.c_str());
            // 只有当飞书正在运行且路径与已保存的路径匹配时才勾选
            bool checked = localFeishuRunning && (g_feishuPath == localFeishuPath);
            if (ImGui::Checkbox("[Save Feishu Path]", &checked))
            {
                if (checked)
                {
                    // 勾选时保存路径
                    g_feishuPath = localFeishuPath;
                }
                else
                {
                    // 取消勾选时清空保存的路径
                    g_feishuPath.clear();
                }
            }
        }
        ImGui::EndGroup();
        
        // WeChat Status
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "WeChat Status");
        ImGui::BeginGroup();
        if (localWechatRunning)
        {
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "✓ Running");
        }
        else
        {
            ImGui::TextColored(ImVec4(0.8f, 0.2f, 0.2f, 1.0f), "✗ Not Running");
        }
        
        // 显示当前检测到的微信路径（如果有的话）
        if (localWechatRunning && !localCurrentWechatPath.empty())
        {
            ImGui::TextWrapped("[Path] %s", localCurrentWechatPath.c_str());
            // 只有当微信正在运行且路径与已保存的路径匹配时才勾选
            bool checked = (g_wechatPath == localCurrentWechatPath);
            if (ImGui::Checkbox("[Save WeChat Path]", &checked))
            {
                if (checked)
                {
                    // 勾选时保存路径
                    g_wechatPath = localCurrentWechatPath;
                }
                else
                {
                    // 取消勾选时清空保存的路径
                    g_wechatPath.clear();
                }
            }
        }
        else if (localWechatRunning)
        {
            ImGui::TextWrapped("[Path] <unknown>");
            // 微信运行但路径未知时，复选框状态取决于是否已有保存的路径
            bool checked = !g_wechatPath.empty();
            if (ImGui::Checkbox("[Save WeChat Path]", &checked))
            {
                if (checked)
                {
                    // 勾选时尝试保存当前检测到的路径（如果有的话）
                    if (!localCurrentWechatPath.empty())
                    {
                        g_wechatPath = localCurrentWechatPath;
                    }
                }
                else
                {
                    // 取消勾选时清空保存的路径
                    g_wechatPath.clear();
                }
            }
        }
        ImGui::EndGroup();
        
        // Middle/Right column: Current Status & Quick Actions
        ImGui::NextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.5f, 1.0f));
        ImGui::Text("⚡ [CONTROL CENTER]");
        ImGui::PopStyleColor();
        
        // 添加控制中心状态指示器
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 20);
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "●");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "READY");
        
        ImGui::Separator();
        
        // Current Status
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "[Current Status]");
        if (!g_currentConfigName.empty())
        {
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Active: %s", g_currentConfigName.c_str());
        }
        else
        {
            ImGui::TextDisabled("No active configuration");
        }
        
        if (!g_selectedSlnPath.empty())
        {
            ImGui::TextWrapped("VS: %s", g_selectedSlnPath.c_str());
        }
        if (!g_selectedCursorFolders.empty())
        {
            ImGui::Text("Cursor selected (%d):", (int)g_selectedCursorFolders.size());
            for (const auto& f : g_selectedCursorFolders)
            {
                ImGui::TextWrapped("- %s", f.c_str());
            }
        }
        
        ImGui::Spacing();
        
        // Quick Actions
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "[Quick Actions]");
        if (!g_selectedSlnPath.empty())
        {
            if (ImGui::Button("[Launch VS]"))
            {
                LaunchVSWithSolution(g_selectedSlnPath);
            }
        }
        // Launch all selected Cursor folders (multi-select)
        if (!g_selectedCursorFolders.empty())
        {
            std::string launchLabel = std::string("[Launch Selected (") + std::to_string((int)g_selectedCursorFolders.size()) + ")]";
            if (ImGui::Button(launchLabel.c_str()))
            {
                for (const auto& folder : g_selectedCursorFolders)
                {
                    LaunchCursorWithFolder(folder);
                }
            }
        }
        else if (!g_selectedCursorFolder.empty())
        {
            // Backward-compatible single selection loaded from config: show a launch button
            if (ImGui::Button("[Launch Cursor]"))
            {
                LaunchCursorWithFolder(g_selectedCursorFolder);
            }
        }
        if (g_selectedSlnPath.empty() && g_selectedCursorFolder.empty())
        {
            ImGui::TextDisabled("Select VS solution or Cursor folder first");
        }
        
        // Launch Feishu
        if (!g_feishuPath.empty())
        {
            if (ImGui::Button("[Launch Feishu]"))
            {
                LaunchFeishu();
            }
        }
        else
        {
            ImGui::TextDisabled("Save Feishu path first");
        }
        
        // Launch WeChat
        if (ImGui::Button("[Launch WeChat]"))
        {
            LaunchWechat();
        }
        
                 // Third column (only in wide layout): Configuration Management
         if (useWideLayout)
         {
             ImGui::NextColumn();
             ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.5f, 1.0f));
             ImGui::Text("💾 [DATA MANAGEMENT]");
             ImGui::PopStyleColor();
             
             // 添加数据管理状态指示器
             ImGui::SameLine();
             ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 20);
             ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "●");
             ImGui::SameLine();
             ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "SYNC");
             
             ImGui::Separator();
             
             // Save/Update configuration
             // 如果需要填充配置名称（从编辑弹窗触发）
             if (g_shouldFillConfigName)
             {
                 strncpy(g_mainConfigNameBuf, g_currentConfigName.c_str(), sizeof(g_mainConfigNameBuf) - 1);
                 g_shouldFillConfigName = false;
             }
             
             ImGui::InputText("Config Name", g_mainConfigNameBuf, sizeof(g_mainConfigNameBuf));
             
             // 根据是否已存在配置来决定按钮文本和行为
             bool configExists = false;
             for (const auto& cfg : g_savedConfigs)
             {
                 if (cfg.name == g_mainConfigNameBuf)
                 {
                     configExists = true;
                     break;
                 }
             }
             
             const char* buttonText = configExists ? "[Update Existing Config]" : "[Save Current as New Config]";
             if (ImGui::Button(buttonText))
             {
                 if (strlen(g_mainConfigNameBuf) > 0)
                 {
                     g_currentConfigName = g_mainConfigNameBuf;
                     SavePrefs();
                     if (configExists)
                     {
                         AppendLog("[prefs] updated config: " + g_currentConfigName);
                     }
                     else
                     {
                         AppendLog("[prefs] saved as new config: " + g_currentConfigName);
                         memset(g_mainConfigNameBuf, 0, sizeof(g_mainConfigNameBuf)); // 只有新建时才清空输入框
                     }
                 }
             }
             
             ImGui::Spacing();
             
             // Load existing configurations (sorted by lastUsedAt desc, then createdAt desc)
             if (!g_savedConfigs.empty())
             {
                 ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "[Saved Configurations]");
                 
                 // 创建可滚动的配置列表区域，使用剩余空间，自定义滚动条样式
                 ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 8.0f); // 设置滚动条宽度为8像素
                 ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0.1f, 0.1f, 0.1f, 0.6f)); // 滚动条背景色
                 ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(0.3f, 0.3f, 0.3f, 0.8f)); // 滚动条抓取器颜色
                 ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(0.4f, 0.4f, 0.4f, 0.9f)); // 悬停时颜色
                 ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImVec4(0.5f, 0.5f, 0.5f, 1.0f)); // 激活时颜色
                 
                 ImGui::BeginChild("ConfigList", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
                 
                 std::vector<SavedConfig> sorted = g_savedConfigs;
                 std::sort(sorted.begin(), sorted.end(), [](const SavedConfig& a, const SavedConfig& b){
                     if (a.lastUsedAt != b.lastUsedAt) return a.lastUsedAt > b.lastUsedAt;
                     return a.createdAt > b.createdAt;
                 });
                 for (const auto& config : sorted)
                 {
                     ImGui::BeginGroup();
                     ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f)); // 亮橙色
                     ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 4));
                     ImGui::Text(" [Config] %s", config.name.c_str());
                     ImGui::PopStyleVar();
                     ImGui::PopStyleColor();
                     if (!config.vsSolutionPath.empty())
                         ImGui::TextWrapped("VS: %s", config.vsSolutionPath.c_str());
                     if (!config.cursorFolderPath.empty())
                         ImGui::TextWrapped("Cursor: %s", config.cursorFolderPath.c_str());
                     if (!config.feishuPath.empty())
                         ImGui::TextWrapped("Feishu: %s", config.feishuPath.c_str());
                     if (!config.wechatPath.empty())
                         ImGui::TextWrapped("WeChat: %s", config.wechatPath.c_str());
                     
                     if (ImGui::Button(("[Load]##" + config.name).c_str()))
                     {
                         // Update last used timestamp then load
                         for (auto &cfg : g_savedConfigs) { if (cfg.name == config.name) { cfg.lastUsedAt = (unsigned long long)time(nullptr); break; } }
                         SavePrefs();
                         LoadConfig(config.name);
                     }
                     ImGui::SameLine();
                     if (ImGui::Button(("[Edit]##" + config.name).c_str()))
                     {
                         // 直接将配置名称填入到主界面的 Config Name 输入框
                         memset(g_mainConfigNameBuf, 0, sizeof(g_mainConfigNameBuf));
                         strncpy(g_mainConfigNameBuf, config.name.c_str(), sizeof(g_mainConfigNameBuf) - 1);
                         g_currentConfigName = config.name;
                         
                         // 加载配置到当前选择
                         LoadConfig(config.name);
                     }
                    ImGui::SameLine();
                    if (ImGui::Button(("[Delete]##" + config.name).c_str()))
                    {
                        // 显示确认对话框
                        ImGui::OpenPopup(("Confirm Delete##" + config.name).c_str());
                    }
                    
                    // 确认删除弹窗
                    if (ImGui::BeginPopupModal(("Confirm Delete##" + config.name).c_str(), NULL, ImGuiWindowFlags_AlwaysAutoResize))
                    {
                        ImGui::Text("Are you sure you want to delete configuration '%s'?", config.name.c_str());
                        ImGui::Text("This action cannot be undone.");
                        ImGui::Separator();
                        
                        if (ImGui::Button("Yes, Delete", ImVec2(120, 0)))
                        {
                            DeleteConfig(config.name);
                            ImGui::CloseCurrentPopup();
                            // 强制UI更新
                            ImGui::SetWindowFocus();
                        }
                        ImGui::SetItemDefaultFocus();
                        ImGui::SameLine();
                        if (ImGui::Button("Cancel", ImVec2(120, 0)))
                        {
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::EndGroup();
                    ImGui::Spacing();
                }
                
                                 // 结束可滚动的配置列表区域
                 ImGui::EndChild();
                 
                 // 恢复滚动条样式
                 ImGui::PopStyleColor(4); // 弹出4个颜色样式
                 ImGui::PopStyleVar(); // 弹出滚动条大小样式
            }
            else
            {
                ImGui::TextDisabled("No saved configurations");
            }
        }
        else
        {
            // In narrow layout, put configuration management in the same column
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "[Configuration Management]");
            
            // Save/Update configuration
            // 如果需要填充配置名称（从编辑弹窗触发）
            if (g_shouldFillConfigName)
            {
                strncpy(g_mainConfigNameBuf, g_currentConfigName.c_str(), sizeof(g_mainConfigNameBuf) - 1);
                g_shouldFillConfigName = false;
            }
            
            ImGui::InputText("Config Name", g_mainConfigNameBuf, sizeof(g_mainConfigNameBuf));
            
            // 根据是否已存在配置来决定按钮文本和行为
            bool configExists = false;
            for (const auto& cfg : g_savedConfigs)
            {
                if (cfg.name == g_mainConfigNameBuf)
                {
                    configExists = true;
                    break;
                }
            }
            
            const char* buttonText = configExists ? "[Update Existing Config]" : "[Save Current as New Config]";
            if (ImGui::Button(buttonText))
            {
                if (strlen(g_mainConfigNameBuf) > 0)
                {
                    g_currentConfigName = g_mainConfigNameBuf;
                    SavePrefs();
                    if (configExists)
                    {
                        AppendLog("[prefs] updated config: " + g_currentConfigName);
                    }
                    else
                    {
                        AppendLog("[prefs] saved as new config: " + g_currentConfigName);
                        memset(g_mainConfigNameBuf, 0, sizeof(g_mainConfigNameBuf)); // 只有新建时才清空输入框
                    }
                }
            }
            
            ImGui::Spacing();
            
            // Load existing configurations (sorted by lastUsedAt desc, then createdAt desc)
            if (!g_savedConfigs.empty())
            {
                ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "[Saved Configurations]");
                std::vector<SavedConfig> sorted = g_savedConfigs;
                std::sort(sorted.begin(), sorted.end(), [](const SavedConfig& a, const SavedConfig& b){
                    if (a.lastUsedAt != b.lastUsedAt) return a.lastUsedAt > b.lastUsedAt;
                    return a.createdAt > b.createdAt;
                });
                for (const auto& config : sorted)
                {
                    ImGui::BeginGroup();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f)); // 亮橙色
                    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 4));
                    ImGui::Text(" [Config] %s", config.name.c_str());
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor();
                    if (!config.vsSolutionPath.empty())
                        ImGui::TextWrapped("VS: %s", config.vsSolutionPath.c_str());
                    if (!config.cursorFolderPath.empty())
                        ImGui::TextWrapped("Cursor: %s", config.cursorFolderPath.c_str());
                    if (!config.feishuPath.empty())
                        ImGui::TextWrapped("Feishu: %s", config.feishuPath.c_str());
                    
                    if (ImGui::Button(("[Load]##" + config.name).c_str()))
                    {
                        // Update last used timestamp then load
                        for (auto &cfg : g_savedConfigs) { if (cfg.name == config.name) { cfg.lastUsedAt = (unsigned long long)time(nullptr); break; } }
                        SavePrefs();
                        LoadConfig(config.name);
                    }
                    ImGui::SameLine();
                                         if (ImGui::Button(("[Edit]##" + config.name).c_str()))
                     {
                         // 直接将配置名称填入到主界面的 Config Name 输入框
                         memset(g_mainConfigNameBuf, 0, sizeof(g_mainConfigNameBuf));
                         strncpy(g_mainConfigNameBuf, config.name.c_str(), sizeof(g_mainConfigNameBuf) - 1);
                         g_currentConfigName = config.name;
                         
                         // 加载配置到当前选择
                         LoadConfig(config.name);
                     }
                    ImGui::SameLine();
                    if (ImGui::Button(("[Delete]##" + config.name).c_str()))
                    {
                        // 显示确认对话框
                        ImGui::OpenPopup(("Confirm Delete##" + config.name).c_str());
                    }
                    
                    // 确认删除弹窗
                    if (ImGui::BeginPopupModal(("Confirm Delete##" + config.name).c_str(), NULL, ImGuiWindowFlags_AlwaysAutoResize))
                    {
                        ImGui::Text("Are you sure you want to delete configuration '%s'?", config.name.c_str());
                        ImGui::Text("This action cannot be undone.");
                        ImGui::Separator();
                        
                        if (ImGui::Button("Yes, Delete", ImVec2(120, 0)))
                        {
                            DeleteConfig(config.name);
                            ImGui::CloseCurrentPopup();
                            // 强制UI更新
                            ImGui::SetWindowFocus();
                        }
                        ImGui::SetItemDefaultFocus();
                        ImGui::SameLine();
                        if (ImGui::Button("Cancel", ImVec2(120, 0)))
                        {
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::EndGroup();
                    ImGui::Spacing();
                }
            }
            else
            {
                ImGui::TextDisabled("No saved configurations");
            }
        }
        
        

        ImGui::Columns(1);
        
        // 系统资源监控（放在下方）
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.5f, 1.0f));
        ImGui::Text("📊 SYSTEM RESOURCES MONITOR");
        ImGui::PopStyleColor();
        
        // 创建两列布局显示系统资源
        ImGui::Columns(2, "SystemResources", true);
        ImGui::SetColumnWidth(0, ImGui::GetWindowWidth() * 0.5f);
        ImGui::SetColumnWidth(1, ImGui::GetWindowWidth() * 0.5f);
        
        // 左列：系统资源
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "CPU:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f), "%.1f%%", g_systemResources.cpuUsage);
        
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "Memory:");
        ImGui::SameLine();
        float memoryUsagePercent = g_systemResources.totalMemory > 0 ? 
            (float)g_systemResources.usedMemory / g_systemResources.totalMemory * 100.0f : 0.0f;
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f), "%.1f%% (%.1fGB/%.1fGB)", 
                          memoryUsagePercent,
                          g_systemResources.usedMemory / (1024.0f * 1024.0f * 1024.0f),
                          g_systemResources.totalMemory / (1024.0f * 1024.0f * 1024.0f));
        
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "Disk C:");
        ImGui::SameLine();
        float diskUsagePercent = g_systemResources.totalDisk > 0 ? 
            (float)(g_systemResources.totalDisk - g_systemResources.freeDisk) / g_systemResources.totalDisk * 100.0f : 0.0f;
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f), "%.1f%% (%.1fGB/%.1fGB)", 
                          diskUsagePercent,
                          (g_systemResources.totalDisk - g_systemResources.freeDisk) / (1024.0f * 1024.0f * 1024.0f),
                          g_systemResources.totalDisk / (1024.0f * 1024.0f * 1024.0f));
        
        // 右列：应用状态和系统信息
        ImGui::NextColumn();
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "Uptime:");
        ImGui::SameLine();
        unsigned long long hours = g_systemResources.uptime / 3600;
        unsigned long long minutes = (g_systemResources.uptime % 3600) / 60;
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f), "%llu:%02llu", hours, minutes);
        
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "VS:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f), "%d running", (int)g_vsList.size());
        
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "Cursor:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f), "%d running", (int)g_cursorList.size());
        
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "Feishu:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f), "%s", g_feishuRunning ? "ONLINE" : "OFFLINE");
        
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "WeChat:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f), "%s", g_wechatRunning ? "ONLINE" : "OFFLINE");
        
        ImGui::Columns(1);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        
        // Footer with legacy controls (collapsible)
        if (ImGui::CollapsingHeader("[Advanced Options]", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::Button("[Save Current Selection]"))
            {
                SavePrefs();
            }
            ImGui::SameLine();
            if (ImGui::Button("[Reload All Configs]"))
            {
                LoadPrefs();
            }
            ImGui::SameLine();
            if (ImGui::Button("[Export JSON]"))
            {
                ImGui::OpenPopup("Export JSON");
            }
            ImGui::SameLine();
            if (ImGui::Button("[Import JSON]"))
            {
                ImGui::OpenPopup("Import JSON");
            }
            // Export modal
            if (ImGui::BeginPopupModal("Export JSON", NULL, ImGuiWindowFlags_AlwaysAutoResize))
            {
                static char exportPath[1024] = "";
                if (exportPath[0] == '\0')
                {
                    std::string def = GetDefaultExportJsonFile().string();
                    strncpy(exportPath, def.c_str(), sizeof(exportPath) - 1);
                }
                ImGui::InputText("File path", exportPath, sizeof(exportPath));
                ImGui::SameLine();
                if (ImGui::SmallButton("Browse..."))
                {
                    char buf[1024] = {0}; strncpy(buf, exportPath, sizeof(buf)-1);
                    if (ShowSaveFileDialog(buf, sizeof(buf), "JSON Files\0*.json\0All Files\0*.*\0\0", "Export Configs", "json"))
                    {
                        strncpy(exportPath, buf, sizeof(exportPath)-1);
                        AppendLog(std::string("[prefs] export path selected: ") + exportPath);
                    }
                }
                if (ImGui::Button("Save", ImVec2(120, 0)))
                {
                    if (strlen(exportPath) > 0) { SaveConfigsToJsonFile(fs::path(exportPath)); }
                    AppendLog(std::string("[prefs] exported to ") + exportPath);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120, 0)))
                {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            // Import modal
            if (ImGui::BeginPopupModal("Import JSON", NULL, ImGuiWindowFlags_AlwaysAutoResize))
            {
                static char importPath[1024] = "";
                ImGui::InputText("File path", importPath, sizeof(importPath));
                ImGui::SameLine();
                if (ImGui::SmallButton("Browse..."))
                {
                    char buf[1024] = {0}; strncpy(buf, importPath, sizeof(buf)-1);
                    if (ShowOpenFileDialog(buf, sizeof(buf), "JSON Files\0*.json\0All Files\0*.*\0\0", "Import Configs"))
                    {
                        strncpy(importPath, buf, sizeof(importPath)-1);
                        AppendLog(std::string("[prefs] import path selected: ") + importPath);
                    }
                }
                if (ImGui::Button("Import", ImVec2(120, 0)))
                {
                    std::vector<SavedConfig> incoming;
                    std::ifstream ifs(std::string(importPath), std::ios::binary);
                    if (ifs)
                    {
                        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                        if (!ParseConfigsFromJson(content, incoming))
                        {
                            AppendLog("[prefs] import failed: invalid JSON content");
                        }
                    }
                    else { AppendLog("[prefs] import failed: cannot open file"); }
                    if (!incoming.empty())
                    {
                        MergeConfigs(g_savedConfigs, incoming);
                        SavePrefs();
                        AppendLog(std::string("[prefs] imported ") + std::to_string((int)incoming.size()) + " config(s) from " + importPath);
                    }
                    else { AppendLog("[prefs] import found 0 valid configs"); }
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120, 0)))
                {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }
        
        // 移除底部状态栏，已移至顶部
        
        // Log section (collapsible)
        if (ImGui::CollapsingHeader("[Debug Log]", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ReplaceTool::DrawSharedLog("vslog", 200.0f);
        }
        
        ImGui::End();
    }
#else
    void Refresh() {}
    void DrawVSUI()
    {
        ImGui::Begin("Running Visual Studio");
        ImGui::TextDisabled("Windows only");
        ImGui::End();
    }
#endif
}


