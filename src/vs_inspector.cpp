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
#include <tlhelp32.h>
#include <unordered_map>
#include <unordered_set>
#include <objbase.h>
#include <oleauto.h>
#include <filesystem>
#include <commdlg.h>
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
    static std::mutex g_vsMutexVS;

    // Configuration structure
    struct SavedConfig
    {
        std::string name;
        std::string vsSolutionPath;
        std::string cursorFolderPath;
        std::string feishuPath;
        unsigned long long createdAt = 0;    // unix time seconds
        unsigned long long lastUsedAt = 0;   // unix time seconds
    };

    // Selections and persistence
    static std::vector<SavedConfig> g_savedConfigs;
    static std::string g_selectedSlnPath;
    static std::string g_selectedCursorFolder;
    static std::unordered_set<std::string> g_selectedCursorFolders;
    static std::string g_currentConfigName;
    static bool g_prefsLoaded = false;  // Track if prefs have been loaded

         // 用于主界面配置名称输入框的全局变量
     static char g_mainConfigNameBuf[256] = {0};
     static bool g_shouldFillConfigName = false;

    // Forward declare env helper used by prefs
    static std::string GetEnvU8(const char* name);
    
    // Forward declare launch helpers
    static bool LaunchVSWithSolution(const std::string& slnPath);
    static bool LaunchCursorWithFolder(const std::string& folderPath);
    static bool LaunchFeishu();

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
            ofs << "      \"createdAt\": " << c.createdAt << ",\n";
            ofs << "      \"lastUsedAt\": " << c.lastUsedAt << "\n";
            ofs << "    }" << (i + 1 < g_savedConfigs.size() ? ",\n" : "\n");
        }
        ofs << "  ]\n}";
        AppendLog(std::string("[prefs] saved JSON ") + std::to_string(g_savedConfigs.size()) + " config(s) to " + p.string());
        if (!g_selectedSlnPath.empty()) AppendLog("[prefs] saved VS solution: " + g_selectedSlnPath);
        if (!g_selectedCursorFolder.empty()) AppendLog("[prefs] saved Cursor folder: " + g_selectedCursorFolder);
        if (!g_feishuPath.empty()) AppendLog("[prefs] saved Feishu path: " + g_feishuPath);
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
                                                 if (k == "name" || k == "vs" || k == "cursor" || k == "feishu")
                         {
                             std::string v; if (!ParseJsonString(content, pos, v)) { pos = content.size(); break; }
                             if (k == "name") c.name = v; else if (k == "vs") c.vsSolutionPath = v; else if (k == "cursor") c.cursorFolderPath = v; else c.feishuPath = v;
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
                 // Keep the multi-select set in sync with single selection
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
        DISPID dispidSolution = 0; OLECHAR* nameSolution = L"Solution";
        if (FAILED(pDisp->GetIDsOfNames(IID_NULL, &nameSolution, 1, LOCALE_USER_DEFAULT, &dispidSolution)))
        {
            AppendLog("[vs] GetIDsOfNames(Solution) failed");
            return false;
        }
        VARIANT resultSolution; VariantInit(&resultSolution); DISPPARAMS noArgs = {0};
        HRESULT hrInvokeSolution = pDisp->Invoke(dispidSolution, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &noArgs, &resultSolution, NULL, NULL);
        if (FAILED(hrInvokeSolution))
        {
            AppendLog(std::string("[vs] Invoke(Solution) failed hr=") + std::to_string((long)hrInvokeSolution));
            return false;
        }
        bool ok = false;
        if (resultSolution.vt == VT_DISPATCH && resultSolution.pdispVal)
        {
            IDispatch* pSolution = resultSolution.pdispVal;
            DISPID dispidFullName = 0; OLECHAR* nameFullName = L"FullName";
            HRESULT hrNameFN = pSolution->GetIDsOfNames(IID_NULL, &nameFullName, 1, LOCALE_USER_DEFAULT, &dispidFullName);
            if (SUCCEEDED(hrNameFN))
            {
                VARIANT resultFullName; VariantInit(&resultFullName);
                HRESULT hrInvokeFN = pSolution->Invoke(dispidFullName, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &noArgs, &resultFullName, NULL, NULL);
                if (SUCCEEDED(hrInvokeFN))
                {
                    if (resultFullName.vt == VT_BSTR && resultFullName.bstrVal)
                    {
                        slnOut = WideToUtf8(resultFullName.bstrVal);
                        AppendLog(std::string("[vs] Solution.FullName=") + slnOut);
                        ok = !slnOut.empty();
                    }
                    else
                    {
                        AppendLog(std::string("[vs] Solution.FullName vt=") + std::to_string((int)resultFullName.vt));
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
        IUnknown* pUnk = NULL; HRESULT hrGetObj = pRot->GetObject(pMoniker, &pUnk);
        if (FAILED(hrGetObj) || !pUnk) { AppendLog(std::string("[vs] GetObject(moniker) failed hr=") + std::to_string((long)hrGetObj)); return; }
        IDispatch* pDisp = NULL; HRESULT hrQI = pUnk->QueryInterface(IID_IDispatch, (void**)&pDisp);
        if (FAILED(hrQI) || !pDisp) { AppendLog(std::string("[vs] QueryInterface(IDispatch) failed hr=") + std::to_string((long)hrQI)); pUnk->Release(); return; }

        DWORD pid = 0; 
        if (!GetPidFromDTE(pDisp, pid) || pid == 0) {
            if (pidHint != 0) {
                pid = pidHint;
                AppendLog(std::string("[vs] Using pidHint from ROT: ") + std::to_string((unsigned long)pid));
            } else {
                AppendLog("[vs] GetPidFromDTE failed and no pidHint"); pDisp->Release(); pUnk->Release(); return;
            }
        }
        std::string sln; bool gotSln = TryGetSolutionFullName(pDisp, sln);
        AppendLog(std::string("[vs] TryGetSolutionFullName result=") + (gotSln?"true":"false") + std::string(" sln=") + (sln.empty()?"<empty>":sln));
        if (sln.empty()) { TryFillFromActiveDocument(pDisp, pid, found, sln); }
        if (!sln.empty())
        {
            for (auto& inst : found) { if (inst.pid == pid) { inst.solutionPath = sln; AppendLog(std::string("[vs] pid ") + std::to_string((unsigned long)pid) + std::string(" Set solutionPath")); } }
        }
        else
        {
            AppendLog(std::string("[vs] pid ") + std::to_string((unsigned long)pid) + std::string(" no solution resolved"));
        }
        pDisp->Release(); pUnk->Release();
    }

    void Refresh()
    {
        AppendLog("[vs] RefreshVSInstances: begin");
        std::vector<VSInstance> found;
        std::vector<CursorInstance> foundCursor;
        std::string foundFeishuPath;
        bool foundFeishuRunning = false;

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
                    AppendLog(std::string("[cursor] found cursor.exe pid=") + std::to_string((unsigned long)cinst.pid) + (cinst.exePath.empty()?" path=<unknown>":std::string(" path=") + cinst.exePath));
                    foundCursor.push_back(cinst);
                }
                else if (exeLower == std::string("feishu.exe") || exeLower == std::string("lark.exe"))
                {
                    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pe.th32ProcessID);
                    if (hProc)
                    {
                        char buf[MAX_PATH];
                        DWORD sz = (DWORD)sizeof(buf);
                        if (QueryFullProcessImageNameA(hProc, 0, buf, &sz))
                            foundFeishuPath.assign(buf, sz);
                        CloseHandle(hProc);
                    }
                    foundFeishuRunning = true;
                    AppendLog(std::string("[feishu] found ") + exeLower + std::string(" pid=") + std::to_string((unsigned long)pe.th32ProcessID) + (foundFeishuPath.empty()?" path=<unknown>":std::string(" path=") + foundFeishuPath));
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

        for (auto& inst : found)
        {
            auto it = pidToTitle.find(inst.pid);
            if (it != pidToTitle.end()) inst.windowTitle = it->second;
            AppendLog(std::string("[vs] pid ") + std::to_string((unsigned long)inst.pid) + std::string(" title=") + (inst.windowTitle.empty()?"<none>":inst.windowTitle));
        }



        // 首先获取所有openedWindows中的文件夹
        std::vector<std::string> openedFolders;
        std::string appdata = GetEnvU8("APPDATA");
        AppendLog(std::string("[cursor] APPDATA=") + appdata);
        if (!appdata.empty())
        {
            fs::path p = fs::path(appdata) / "Cursor" / "User" / "globalStorage" / "storage.json";
            AppendLog(std::string("[cursor] checking storage.json at: ") + p.string());
            std::error_code ec; 
            if (fs::exists(p, ec))
            {
                AppendLog("[cursor] storage.json exists, reading content...");
                std::ifstream ifs(p.string(), std::ios::binary);
                if (ifs)
                {
                    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                    AppendLog(std::string("[cursor] storage.json content size: ") + std::to_string(content.size()));
                    size_t openedWindowsPos = content.find("\"openedWindows\"");
                    if (openedWindowsPos != std::string::npos)
                    {
                        AppendLog(std::string("[cursor] found openedWindows at position: ") + std::to_string(openedWindowsPos));
                        size_t pos = openedWindowsPos;
                        int folderCount = 0;
                        while (true)
                        {
                            size_t folderPos = content.find("\"folder\"", pos);
                            if (folderPos == std::string::npos) 
                            {
                                AppendLog(std::string("[cursor] no more folder entries found, total found: ") + std::to_string(folderCount));
                                break;
                            }
                            
                            folderCount++;
                            AppendLog(std::string("[cursor] found folder entry #") + std::to_string(folderCount) + std::string(" at position: ") + std::to_string(folderPos));
                            
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
                            AppendLog(std::string("[cursor] folder URI: ") + folderUri);
                            
                            std::string winPath;
                            if (DecodeFileUriToWindowsPath(folderUri, winPath))
                            {
                                openedFolders.push_back(winPath);
                                AppendLog(std::string("[cursor] decoded opened folder: ") + winPath);
                            }
                            else
                            {
                                AppendLog(std::string("[cursor] failed to decode folder URI: ") + folderUri);
                            }
                            pos = valueEnd + 1;
                        }
                    }
                    else
                    {
                        AppendLog("[cursor] openedWindows not found in storage.json");
                    }
                }
                else
                {
                    AppendLog("[cursor] failed to open storage.json for reading");
                }
            }
            else
            {
                AppendLog(std::string("[cursor] storage.json does not exist, error code: ") + std::to_string(ec.value()));
            }
        }
        else
        {
            AppendLog("[cursor] APPDATA environment variable is empty");
        }
        AppendLog(std::string("[cursor] total openedFolders found: ") + std::to_string(openedFolders.size()));
        AppendLog(std::string("[cursor] total cursor processes found: ") + std::to_string(foundCursor.size()));

        // 如果没有从 openedWindows 解析到任何文件夹，则尝试从 lastActiveWindow 读取一个文件夹作为回退
        if (openedFolders.empty())
        {
            AppendLog("[cursor] openedFolders empty, trying lastActiveWindow as fallback...");
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
                        AppendLog(std::string("[cursor] lastActiveWindowPos: ") + std::to_string(lastActiveWindowPos));
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
                                                AppendLog(std::string("[cursor] fallback lastActiveWindow folder: ") + winPath2);
                                            }
                                            else
                                            {
                                                AppendLog(std::string("[cursor] failed to decode lastActiveWindow folder URI: ") + folderUri);
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
            AppendLog(std::string("[cursor] pid ") + std::to_string((unsigned long)cinst.pid) + std::string(" title=") + (cinst.windowTitle.empty()?"<none>":cinst.windowTitle));
            
            // 直接分配openedFolders中的文件夹，按索引顺序
            if (idx < openedFolders.size())
            {
                cinst.folderPath = openedFolders[idx];
                fs::path folderPath = cinst.folderPath;
                cinst.workspaceName = folderPath.filename().string();
                AppendLog(std::string("[cursor] pid ") + std::to_string((unsigned long)cinst.pid) + std::string(" directly assigned opened folder ") + cinst.folderPath + std::string(" (index ") + std::to_string(idx) + std::string(")"));
            }
            else
            {
                AppendLog(std::string("[cursor] pid ") + std::to_string((unsigned long)cinst.pid) + std::string(" no opened folder available (index ") + std::to_string(idx) + std::string(" >= ") + std::to_string(openedFolders.size()) + std::string(")"));
            }
        }

        HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        bool didCoInit = SUCCEEDED(hr);
        AppendLog(std::string("[vs] CoInitializeEx hr=") + std::to_string((long)hr) + std::string(" didCoInit=") + (didCoInit?"true":"false"));
        static bool comSecurityInitialized = false;
        if (didCoInit && !comSecurityInitialized)
        {
            HRESULT hrSec = CoInitializeSecurity(NULL, -1, NULL, NULL,
                RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
            AppendLog(std::string("[vs] CoInitializeSecurity hr=") + std::to_string((long)hrSec));
            if (SUCCEEDED(hrSec)) comSecurityInitialized = true;
        }
        IRunningObjectTable* pRot = NULL;
        IEnumMoniker* pEnum = NULL;
        if (SUCCEEDED(GetRunningObjectTable(0, &pRot)) && pRot)
        {
            AppendLog("[vs] Got ROT");
            if (SUCCEEDED(pRot->EnumRunning(&pEnum)) && pEnum)
            {
                AppendLog("[vs] EnumRunning success");
                IMoniker* pMoniker = NULL;
                IBindCtx* pBindCtx = NULL;
                CreateBindCtx(0, &pBindCtx);
                while (pEnum->Next(1, &pMoniker, NULL) == S_OK)
                {
                    LPOLESTR displayName = NULL;
                    if (pBindCtx && SUCCEEDED(pMoniker->GetDisplayName(pBindCtx, NULL, &displayName)) && displayName)
                    {
                        std::wstring dn(displayName);
                        if (dn.find(L"!VisualStudio.DTE") == 0)
                        {
                            AppendLog(std::string("[vs] ROT item: ") + WideToUtf8(dn));
                            DWORD pidHint = 0; (void)ParsePidFromRotName(dn, pidHint);
                            if (pidHint != 0) AppendLog(std::string("[vs] pidHint from ROT=") + std::to_string((unsigned long)pidHint));
                            AppendLog("[vs] ProcessDteMoniker: begin");
                            // Use flattened helper to avoid deep nesting
                            ProcessDteMoniker(pRot, pMoniker, found, pidHint);
                            AppendLog(std::string("[vs] ProcessDteMoniker: end; instances now=") + std::to_string((int)found.size()));
                            if (displayName) CoTaskMemFree(displayName);
                            if (pMoniker) pMoniker->Release();
                            continue;
                        }
                    }
                    if (displayName) CoTaskMemFree(displayName);
                    if (pMoniker) pMoniker->Release();
                }
                if (pBindCtx) pBindCtx->Release();
                pEnum->Release();
            }
            pRot->Release();
        }
        if (didCoInit) CoUninitialize();

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
        }
        AppendLog(std::string("[vs] RefreshVSInstances: end, instances=") + std::to_string((int)g_vsList.size()));
        AppendLog(std::string("[cursor] RefreshCursorInstances: end, instances=") + std::to_string((int)g_cursorList.size()));
        AppendLog(std::string("[feishu] Status: ") + (g_feishuRunning ? "Running" : "Not running") + (g_feishuPath.empty() ? "" : " Path: " + g_feishuPath));
        for (const auto& inst : g_vsList)
        {
            AppendLog(std::string("[vs] summary pid=") + std::to_string((unsigned long)inst.pid)
                + std::string(" title=") + (inst.windowTitle.empty()?"<none>":inst.windowTitle)
                + std::string(" solution=") + (inst.solutionPath.empty()?"<none>":inst.solutionPath)
                + std::string(" activeDoc=") + (inst.activeDocumentPath.empty()?"<none>":inst.activeDocumentPath));
        }
    }

    void DrawVSUI()
    {
        // Ensure preferences are loaded on first UI draw
        EnsurePrefsLoaded();
        
        // 设置窗口为可调整大小，并设置最小尺寸
        ImGui::SetNextWindowSizeConstraints(ImVec2(800, 600), ImVec2(FLT_MAX, FLT_MAX));
        ImGui::Begin(" VS & Cursor & Feishu Manager 🚀", nullptr, ImGuiWindowFlags_None);
        
        // Header with refresh controls
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "VS & Cursor & Feishu Manager 😊");
        ImGui::SameLine();
        if (ImGui::Button("[Refresh]"))
        {
            ReplaceTool::AppendLog("[vs] UI: Refresh clicked");
            Refresh();
        }
        
        ImGui::Separator();
        
        // 获取当前窗口宽度，动态调整布局
        float windowWidth = ImGui::GetWindowWidth();
        bool useWideLayout = windowWidth > 1200;
        
        if (useWideLayout)
        {
            // 宽屏布局：三列
            ImGui::Columns(3, "MainContent", true);
        }
        else
        {
            // 窄屏布局：两列
            ImGui::Columns(2, "MainContent", true);
        }
        
        // Left column: Running Instances
        ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "[Running Instances]");
        ImGui::Separator();
        
        std::vector<VSInstance> local;
        std::vector<CursorInstance> localCursor;
        std::string localFeishuPath;
        bool localFeishuRunning;
        {
            std::lock_guard<std::mutex> lock(g_vsMutexVS);
            local = g_vsList;
            localCursor = g_cursorList;
            localFeishuPath = g_feishuPath;
            localFeishuRunning = g_feishuRunning;
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
                    bool checked = (g_selectedSlnPath == inst.solutionPath);
                    std::string chkId = std::string("[Use this solution]##") + std::to_string((unsigned long)inst.pid);
                    if (ImGui::Checkbox(chkId.c_str(), &checked))
                    {
                        g_selectedSlnPath = checked ? inst.solutionPath : std::string();
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
            bool checked = !g_feishuPath.empty();
            if (ImGui::Checkbox("[Save Feishu Path]", &checked))
            {
                g_feishuPath = checked ? localFeishuPath : std::string();
            }
        }
        ImGui::EndGroup();
        
        // Middle/Right column: Current Status & Quick Actions
        ImGui::NextColumn();
        ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "[Current Status & Actions]");
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
        
                 // Third column (only in wide layout): Configuration Management
         if (useWideLayout)
         {
             ImGui::NextColumn();
             ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "[Configuration Management]");
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

