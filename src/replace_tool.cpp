#include "replace_tool.h"

#include "imgui.h"
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdio>
#include <chrono>
#include <ctime>
#ifdef _WIN32
#include <windows.h>
#include <shobjidl.h>
#include <objbase.h>
#endif

namespace fs = std::filesystem;

namespace ReplaceTool
{
    struct ReplaceState
    {
        std::string directoryPath;
        std::string sourceString;
        std::string targetString;
        bool includeContents = true;
        bool includeFilenames = true;
        bool recurseSubdirectories = true;
        bool backupBeforeRun = true;
        bool writeLogToFile = true;
        std::atomic<bool> isRunning{false};
        std::atomic<bool> cancelRequested{false};
        std::thread worker;
        std::vector<std::string> logLines;
        std::mutex logMutex;
        size_t filesProcessed = 0;
        size_t filesModified = 0;
        size_t namesRenamed = 0;
        std::string lastBackupPath;
        std::string logFilePath;
        std::ofstream logFile;
    };

    static ReplaceState g_state;

    void AppendLog(const std::string& line)
    {
        std::lock_guard<std::mutex> lock(g_state.logMutex);
        g_state.logLines.emplace_back(line);
        if (g_state.logFile.is_open())
        {
            g_state.logFile << line << '\n';
            g_state.logFile.flush();
        }
        static std::ofstream globalLog;
        if (!globalLog.is_open())
        {
            globalLog.open("DearImGuiExample.log", std::ios::out | std::ios::app);
        }
        if (globalLog.is_open())
        {
            globalLog << line << '\n';
            globalLog.flush();
        }
    }

    static std::string ReplaceAll(std::string input, const std::string& from, const std::string& to)
    {
        if (from.empty()) return input;
        size_t pos = 0;
        while ((pos = input.find(from, pos)) != std::string::npos)
        {
            input.replace(pos, from.length(), to);
            pos += to.length();
        }
        return input;
    }

    static std::string MakeTimestamp()
    {
        std::time_t t = std::time(nullptr);
        std::tm tmv;
    #ifdef _WIN32
        localtime_s(&tmv, &t);
    #else
        localtime_r(&t, &tmv);
    #endif
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d",
            tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        return std::string(buf);
    }

    static bool CreateBackup(const fs::path& srcDir, fs::path& outBackupPath)
    {
        if (!fs::exists(srcDir) || !fs::is_directory(srcDir)) return false;
        const std::string ts = MakeTimestamp();
        const std::string name = srcDir.filename().empty() ? std::string("backup_") + ts : (srcDir.filename().string() + std::string("_backup_") + ts);
        fs::path backupDir = srcDir.parent_path() / name;
        std::error_code ec;
        fs::create_directories(backupDir, ec);
        if (ec) { AppendLog(std::string("[error] Create backup dir failed: ") + backupDir.string()); return false; }
        fs::copy(srcDir, backupDir, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
        if (ec)
        {
            AppendLog(std::string("[error] Backup copy failed: ") + ec.message());
            return false;
        }
        outBackupPath = backupDir;
        return true;
    }

    #ifdef _WIN32
    static std::string WideToUtf8(const std::wstring& w)
    {
        if (w.empty()) return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

    static bool PickFolderWin32(std::string& outFolder)
    {
        HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        bool needUninit = SUCCEEDED(hr);
        IFileDialog* pfd = NULL;
        HRESULT h = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd));
        if (FAILED(h)) { if (needUninit) CoUninitialize(); return false; }
        DWORD opts = 0; pfd->GetOptions(&opts); pfd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        h = pfd->Show(NULL);
        if (FAILED(h)) { pfd->Release(); if (needUninit) CoUninitialize(); return false; }
        IShellItem* psi = NULL;
        h = pfd->GetResult(&psi);
        if (FAILED(h) || !psi) { if (psi) psi->Release(); pfd->Release(); if (needUninit) CoUninitialize(); return false; }
        PWSTR pszPath = NULL;
        h = psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
        if (SUCCEEDED(h) && pszPath)
        {
            std::wstring wpath(pszPath);
            outFolder = WideToUtf8(wpath);
            CoTaskMemFree(pszPath);
        }
        psi->Release();
        pfd->Release();
        if (needUninit) CoUninitialize();
        return !outFolder.empty();
    }
    #endif

    static bool ReplaceInFile(const fs::path& filePath, const std::string& from, const std::string& to)
    {
        std::ifstream in(filePath, std::ios::in | std::ios::binary);
        if (!in) return false;
        std::ostringstream ss;
        ss << in.rdbuf();
        std::string content = ss.str();
        in.close();

        std::string replaced = ReplaceAll(content, from, to);
        if (replaced == content) return false;

        std::ofstream out(filePath, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(replaced.data(), static_cast<std::streamsize>(replaced.size()));
        out.close();
        return true;
    }

    static void CollectPaths(const fs::path& root, bool recurse, std::vector<fs::path>& filesOut, std::vector<fs::path>& dirsOut)
    {
        std::error_code ec;
        if (!fs::exists(root, ec)) return;
        if (recurse)
        {
            for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec))
            {
                if (ec) { ec.clear(); continue; }
                const fs::directory_entry& e = *it;
                if (e.is_regular_file(ec)) filesOut.push_back(e.path());
                else if (e.is_directory(ec)) dirsOut.push_back(e.path());
            }
        }
        else
        {
            for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec))
            {
                if (ec) { ec.clear(); continue; }
                const fs::directory_entry& e = *it;
                if (e.is_regular_file(ec)) filesOut.push_back(e.path());
                else if (e.is_directory(ec)) dirsOut.push_back(e.path());
            }
        }
    }

    static void RunReplacement()
    {
        const fs::path root = g_state.directoryPath;
        const std::string from = g_state.sourceString;
        const std::string to = g_state.targetString;

        g_state.filesProcessed = 0;
        g_state.filesModified = 0;
        g_state.namesRenamed = 0;

        if (from.empty())
        {
            AppendLog("[error] Empty source string");
            return;
        }
        std::error_code ec;
        if (!fs::exists(root, ec) || !fs::is_directory(root, ec))
        {
            AppendLog(std::string("[error] Directory not found or inaccessible: ") + root.string());
            return;
        }

        bool logOpened = false;
        if (g_state.writeLogToFile)
        {
            const std::string ts = MakeTimestamp();
            fs::path logPath = root / (std::string("replace_log_") + ts + ".txt");
            g_state.logFilePath = logPath.string();
            g_state.logFile.open(logPath, std::ios::out | std::ios::app);
            if (g_state.logFile.is_open())
            {
                logOpened = true;
                AppendLog(std::string("[info] Logging to: ") + g_state.logFilePath);
                AppendLog(std::string("[info] Options: contents=") + (g_state.includeContents?"on":"off") +
                          ", names=" + (g_state.includeFilenames?"on":"off") +
                          ", recurse=" + (g_state.recurseSubdirectories?"on":"off") +
                          ", backup=" + (g_state.backupBeforeRun?"on":"off"));
            }
            else
            {
                AppendLog("[warn] Could not open log file; continuing with in-memory log only");
            }
        }

        if (g_state.backupBeforeRun)
        {
            fs::path backupPath;
            if (CreateBackup(root, backupPath))
            {
                g_state.lastBackupPath = backupPath.string();
                AppendLog(std::string("[info] Backup created at: ") + g_state.lastBackupPath);
            }
            else
            {
                AppendLog("[error] Backup failed. Aborting.");
                if (logOpened) { g_state.logFile.close(); }
                return;
            }
        }

        std::vector<fs::path> files;
        std::vector<fs::path> dirs;
        CollectPaths(root, g_state.recurseSubdirectories, files, dirs);

        AppendLog("[info] Scan done, files: " + std::to_string(files.size()) + ", dirs: " + std::to_string(dirs.size()));

        if (g_state.includeContents)
        {
            for (const fs::path& p : files)
            {
                if (g_state.cancelRequested) { AppendLog("[warn] Cancelled"); if (logOpened) { g_state.logFile.close(); } return; }
                g_state.filesProcessed++;
                bool modified = false;
                try { modified = ReplaceInFile(p, from, to); }
                catch (...) { AppendLog(std::string("[error] Write failed: ") + p.string()); }
                if (modified)
                {
                    g_state.filesModified++;
                    AppendLog(std::string("[ok] Content replaced: ") + p.string());
                }
            }
        }

        if (g_state.includeFilenames)
        {
            for (const fs::path& p : files)
            {
                if (g_state.cancelRequested) { AppendLog("[warn] Cancelled"); if (logOpened) { g_state.logFile.close(); } return; }
                const std::string name = p.filename().string();
                if (name.find(from) != std::string::npos)
                {
                    fs::path newPath = p.parent_path() / ReplaceAll(name, from, to);
                    std::error_code rec;
                    if (newPath != p)
                    {
                        fs::rename(p, newPath, rec);
                        if (!rec)
                        {
                            g_state.namesRenamed++;
                            AppendLog(std::string("[ok] Renamed file: ") + p.string() + " -> " + newPath.string());
                        }
                        else
                        {
                            AppendLog(std::string("[error] Rename failed: ") + p.string());
                        }
                    }
                }
            }
            std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b){ return a.string().size() > b.string().size(); });
            for (const fs::path& d : dirs)
            {
                if (g_state.cancelRequested) { AppendLog("[warn] Cancelled"); if (logOpened) { g_state.logFile.close(); } return; }
                const std::string name = d.filename().string();
                if (name.find(from) != std::string::npos)
                {
                    fs::path newPath = d.parent_path() / ReplaceAll(name, from, to);
                    std::error_code rec;
                    if (newPath != d)
                    {
                        fs::rename(d, newPath, rec);
                        if (!rec)
                        {
                            g_state.namesRenamed++;
                            AppendLog(std::string("[ok] Renamed dir: ") + d.string() + " -> " + newPath.string());
                        }
                        else
                        {
                            AppendLog(std::string("[error] Rename dir failed: ") + d.string());
                        }
                    }
                }
            }
        }

        AppendLog("[done] Done");
        if (logOpened) { g_state.logFile.close(); }
    }

    void DrawReplaceUI()
    {
        ImGui::Begin("String Replace Tool");
        ImGui::Text("Replace strings in contents and file/dir names under a directory");

        static char dirBuf[1024] = {0};
        static char srcBuf[256] = {0};
        static char dstBuf[256] = {0};

        if (g_state.directoryPath.size() >= sizeof(dirBuf)) g_state.directoryPath.resize(sizeof(dirBuf) - 1);
        if (g_state.sourceString.size() >= sizeof(srcBuf)) g_state.sourceString.resize(sizeof(srcBuf) - 1);
        if (g_state.targetString.size() >= sizeof(dstBuf)) g_state.targetString.resize(sizeof(dstBuf) - 1);
        std::snprintf(dirBuf, sizeof(dirBuf), "%s", g_state.directoryPath.c_str());
        std::snprintf(srcBuf, sizeof(srcBuf), "%s", g_state.sourceString.c_str());
        std::snprintf(dstBuf, sizeof(dstBuf), "%s", g_state.targetString.c_str());

        if (ImGui::InputText("Directory", dirBuf, sizeof(dirBuf))) g_state.directoryPath = dirBuf;
    #ifdef _WIN32
        ImGui::SameLine();
        if (ImGui::Button("Browse..."))
        {
            std::string sel;
            if (PickFolderWin32(sel)) g_state.directoryPath = sel;
        }
    #endif
        if (ImGui::InputText("Source", srcBuf, sizeof(srcBuf))) g_state.sourceString = srcBuf;
        if (ImGui::InputText("Target", dstBuf, sizeof(dstBuf))) g_state.targetString = dstBuf;

        ImGui::Checkbox("Replace file contents", &g_state.includeContents);
        ImGui::SameLine();
        ImGui::Checkbox("Rename files/dirs", &g_state.includeFilenames);
        ImGui::SameLine();
        ImGui::Checkbox("Recurse subdirs", &g_state.recurseSubdirectories);

        ImGui::Checkbox("Backup before run", &g_state.backupBeforeRun);
        if (!g_state.lastBackupPath.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("Last backup: %s", g_state.lastBackupPath.c_str());
        }
        ImGui::Checkbox("Write log to file", &g_state.writeLogToFile);
        if (!g_state.logFilePath.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("Log: %s", g_state.logFilePath.c_str());
        }

        if (!g_state.isRunning)
        {
            if (ImGui::Button("Start"))
            {
                g_state.cancelRequested = false;
                g_state.isRunning = true;
                {
                    std::lock_guard<std::mutex> lock(g_state.logMutex);
                    g_state.logLines.clear();
                }
                g_state.worker = std::thread([](){ RunReplacement(); g_state.isRunning = false; });
                g_state.worker.detach();
            }
        }
        else
        {
            if (ImGui::Button("Cancel"))
            {
                g_state.cancelRequested = true;
            }
            ImGui::SameLine();
            ImGui::Text("Processing... processed %llu, modified %llu, renamed %llu",
                (unsigned long long)g_state.filesProcessed,
                (unsigned long long)g_state.filesModified,
                (unsigned long long)g_state.namesRenamed);
        }

        ImGui::Separator();
        ImGui::Text("Log:");
        ImGui::BeginChild("log", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
        {
            std::lock_guard<std::mutex> lock(g_state.logMutex);
            for (const std::string& line : g_state.logLines)
            {
                ImGui::TextUnformatted(line.c_str());
            }
            if (!g_state.logLines.empty()) ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
        ImGui::End();
    }

    void DrawSharedLog(const char* id, float height)
    {
        ImGui::BeginChild(id, ImVec2(0, height), true, ImGuiWindowFlags_HorizontalScrollbar);
        {
            std::lock_guard<std::mutex> lock(g_state.logMutex);
            for (const std::string& line : g_state.logLines)
            {
                ImGui::TextUnformatted(line.c_str());
            }
            if (!g_state.logLines.empty()) ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }
}
