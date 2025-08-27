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
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <unordered_map>
#include <objbase.h>
#include <oleauto.h>
#include <filesystem>
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
    static std::mutex g_vsMutexVS;

    // Selections and persistence
    static std::string g_selectedSlnPath;
    static std::string g_selectedCursorFolder;
    static bool g_prefsLoaded = false;  // Track if prefs have been loaded

    // Forward declare env helper used by prefs
    static std::string GetEnvU8(const char* name);
    
    // Forward declare launch helpers
    static bool LaunchVSWithSolution(const std::string& slnPath);
    static bool LaunchCursorWithFolder(const std::string& folderPath);

    static fs::path GetPrefsFile()
    {
        std::string appdata = GetEnvU8("APPDATA");
        fs::path dir = appdata.empty() ? fs::path(".") : fs::path(appdata);
        dir /= "DearImGuiTool";
        std::error_code ec; fs::create_directories(dir, ec);
        return dir / "prefs.txt";
    }

    static void SavePrefs()
    {
        fs::path p = GetPrefsFile();
        std::ofstream ofs(p.string(), std::ios::binary);
        if (!ofs) { AppendLog(std::string("[prefs] open for write failed: ") + p.string()); return; }
        ofs << "sln=" << g_selectedSlnPath << "\n";
        ofs << "cursor=" << g_selectedCursorFolder << "\n";
        AppendLog(std::string("[prefs] saved to ") + p.string());
        if (!g_selectedSlnPath.empty())
            AppendLog("[prefs] saved VS solution: " + g_selectedSlnPath);
        if (!g_selectedCursorFolder.empty())
            AppendLog("[prefs] saved Cursor folder: " + g_selectedCursorFolder);
    }

    static void LoadPrefs()
    {
        fs::path p = GetPrefsFile();
        std::error_code ec; if (!fs::exists(p, ec)) { AppendLog("[prefs] no prefs file"); return; }
        std::ifstream ifs(p.string(), std::ios::binary);
        if (!ifs) { AppendLog(std::string("[prefs] open for read failed: ") + p.string()); return; }
        std::string line;
        while (std::getline(ifs, line))
        {
            if (line.rfind("sln=", 0) == 0) g_selectedSlnPath = line.substr(4);
            else if (line.rfind("cursor=", 0) == 0) g_selectedCursorFolder = line.substr(7);
        }
        AppendLog(std::string("[prefs] loaded from ") + p.string());
        if (!g_selectedSlnPath.empty())
            AppendLog("[prefs] loaded VS solution: " + g_selectedSlnPath);
        if (!g_selectedCursorFolder.empty())
            AppendLog("[prefs] loaded Cursor folder: " + g_selectedCursorFolder);
        g_prefsLoaded = true;
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

    static void TryMapCursorWorkspaceFromRecent(const std::string& workspaceName, std::string& folderOut)
    {
        folderOut.clear();
        // Cursor (VS Code派生) 近期记录大概率在 %APPDATA%\Cursor\User\globalStorage\storage.json
        std::string appdata = GetEnvU8("APPDATA");
        if (appdata.empty()) return;
        fs::path p = fs::path(appdata) / "Cursor" / "User" / "globalStorage" / "storage.json";
        std::error_code ec; if (!fs::exists(p, ec)) return;
        std::ifstream ifs(p.string(), std::ios::binary);
        if (!ifs) return;
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        // 粗略提取: 查找 "folderUri":"file:///..." 与紧邻的 "label":"..."
        size_t pos = 0; size_t hitCount = 0;
        while (true)
        {
            size_t fpos = content.find("\"folderUri\"", pos);
            if (fpos == std::string::npos) break;
            size_t q1 = content.find('"', fpos + 12); if (q1 == std::string::npos) break; // after key
            size_t q2 = content.find('"', q1 + 1); if (q2 == std::string::npos) break; // value start quote
            // value
            size_t v1 = content.find('"', q2 + 1); if (v1 == std::string::npos) break;
            std::string folderUri = content.substr(q2 + 1, v1 - (q2 + 1));
            // find label within small window
            size_t lpos = content.find("\"label\"", v1);
            if (lpos == std::string::npos) { pos = v1 + 1; continue; }
            size_t lq1 = content.find('"', lpos + 7); if (lq1 == std::string::npos) { pos = lpos + 1; continue; }
            size_t lq2 = content.find('"', lq1 + 1); if (lq2 == std::string::npos) { pos = lq1 + 1; continue; }
            std::string label = content.substr(lq1 + 1, lq2 - (lq1 + 1));
            // 比较工作区名
            if (_stricmp(label.c_str(), workspaceName.c_str()) == 0)
            {
                std::string winPath;
                if (DecodeFileUriToWindowsPath(folderUri, winPath))
                {
                    folderOut = winPath; hitCount++;
                    break;
                }
            }
            pos = lq2 + 1;
        }
        if (!folderOut.empty())
        {
            AppendLog(std::string("[cursor] recent map: ") + workspaceName + std::string(" -> ") + folderOut);
        }
        else
        {
            AppendLog(std::string("[cursor] recent map miss for ") + workspaceName + std::string(" at ") + p.string());
            std::string winPath;
            if (ExtractLastFileUriWindowsPath(content, winPath))
            {
                folderOut = winPath;
                AppendLog(std::string("[cursor] fallback last workspace -> ") + folderOut);
            }
        }
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
            } while (Process32Next(hSnap, &pe));
        }
        CloseHandle(hSnap);

        std::unordered_map<DWORD, std::string> pidToTitle;
        EnumWindows([](HWND hWnd, LPARAM lParam)->BOOL{
            if (!IsWindowVisible(hWnd)) return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(hWnd, &pid);
            if (pid == 0) return TRUE;
            char title[512];
            int len = GetWindowTextA(hWnd, title, (int)sizeof(title));
            if (len > 0)
            {
                auto* mapPtr = reinterpret_cast<std::unordered_map<DWORD, std::string>*>(lParam);
                auto it = mapPtr->find(pid);
                if (it == mapPtr->end() || (int)it->second.size() < len)
                    (*mapPtr)[pid] = std::string(title, len);
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&pidToTitle));

        for (auto& inst : found)
        {
            auto it = pidToTitle.find(inst.pid);
            if (it != pidToTitle.end()) inst.windowTitle = it->second;
            AppendLog(std::string("[vs] pid ") + std::to_string((unsigned long)inst.pid) + std::string(" title=") + (inst.windowTitle.empty()?"<none>":inst.windowTitle));
        }

        for (auto& cinst : foundCursor)
        {
            auto it = pidToTitle.find(cinst.pid);
            if (it != pidToTitle.end()) cinst.windowTitle = it->second;
            AppendLog(std::string("[cursor] pid ") + std::to_string((unsigned long)cinst.pid) + std::string(" title=") + (cinst.windowTitle.empty()?"<none>":cinst.windowTitle));
            if (cinst.windowTitle.empty())
            {
                // No title to parse; skip heuristics for this instance
                continue;
            }
            // Heuristic: title format "<file> - <workspace> - Cursor" 或 "<folder> - Cursor"
            std::string title = cinst.windowTitle;
            std::string suffix = " - Cursor";
            // Allow admin suffix
            if (title.size() > 9 && title.rfind(" - Cursor (Admin)") == title.size() - 17) {
                title = title.substr(0, title.size() - 17);
            }
            AppendLog(std::string("[cursor] pid ") + std::to_string((unsigned long)cinst.pid) + std::string(" normalizedTitle=") + title);
            size_t pos = title.rfind(suffix);
            std::string head = (pos != std::string::npos) ? title.substr(0, pos) : title;
            // Trim spaces
            while (!head.empty() && (head.back()==' '||head.back()=='\t')) head.pop_back();
            while (!head.empty() && (head.front()==' '||head.front()=='\t')) head.erase(head.begin());
            AppendLog(std::string("[cursor] pid ") + std::to_string((unsigned long)cinst.pid) + std::string(" head=") + head);
            // If head looks like a path and exists as directory, accept
            std::error_code ecx;
            if (!head.empty() && head.find(':') != std::string::npos)
            {
                bool isdir = fs::is_directory(head, ecx);
                AppendLog(std::string("[cursor] pid ") + std::to_string((unsigned long)cinst.pid) + std::string(" checkDir path=") + head + std::string(" isdir=") + (isdir?"true":"false") + std::string(" ec=") + std::to_string((int)ecx.value()));
                if (isdir)
                {
                    cinst.folderPath = head;
                }
            }
            // If still empty,尝试用“ - ”拆分，取最后一个段作为 workspace 名称
            if (cinst.folderPath.empty() && !head.empty())
            {
                // split by " - "
                std::vector<std::string> parts; parts.reserve(3);
                size_t start = 0; while (true) {
                    size_t p = head.find(" - ", start);
                    if (p == std::string::npos) { parts.push_back(head.substr(start)); break; }
                    parts.push_back(head.substr(start, p - start));
                    start = p + 3;
                }
                if (!parts.empty())
                {
                    std::string partsLog;
                    for (size_t i = 0; i < parts.size(); ++i) { if (i) partsLog += " | "; partsLog += parts[i]; }
                    AppendLog(std::string("[cursor] pid ") + std::to_string((unsigned long)cinst.pid) + std::string(" parts=") + partsLog);
                    cinst.workspaceName = parts.back();
                    // trim
                    while (!cinst.workspaceName.empty() && (cinst.workspaceName.back()==' '||cinst.workspaceName.back()=='\t')) cinst.workspaceName.pop_back();
                    while (!cinst.workspaceName.empty() && (cinst.workspaceName.front()==' '||cinst.workspaceName.front()=='\t')) cinst.workspaceName.erase(cinst.workspaceName.begin());
                    AppendLog(std::string("[cursor] pid ") + std::to_string((unsigned long)cinst.pid) + std::string(" workspaceName= ") + cinst.workspaceName);
                    // 尝试从 Cursor 最近工作区记录映射真实文件夹
                    if (!cinst.workspaceName.empty())
                    {
                        std::string mapped;
                        TryMapCursorWorkspaceFromRecent(cinst.workspaceName, mapped);
                        if (!mapped.empty())
                        {
                            cinst.folderPath = mapped;
                        }
                    }
                }
            }
            if (cinst.folderPath.empty())
            {
                AppendLog(std::string("[cursor] pid ") + std::to_string((unsigned long)cinst.pid) + std::string(" cannot parse folder from title: ") + title);
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
        }
        AppendLog(std::string("[vs] RefreshVSInstances: end, instances=") + std::to_string((int)g_vsList.size()));
        AppendLog(std::string("[cursor] RefreshCursorInstances: end, instances=") + std::to_string((int)g_cursorList.size()));
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
        
        ImGui::Begin("Running Visual Studio");
        if (ImGui::Button("Refresh"))
        {
            ReplaceTool::AppendLog("[vs] UI: Refresh clicked");
            Refresh();
        }
        ImGui::SameLine();
        if (ImGui::Button("Auto-Refresh"))
        {
            Refresh();
        }

        std::vector<VSInstance> local;
        std::vector<CursorInstance> localCursor;
        {
            std::lock_guard<std::mutex> lock(g_vsMutexVS);
            local = g_vsList;
            localCursor = g_cursorList;
        }

        ImGui::Text("Found %d instance(s)", (int)local.size());
        ImGui::Separator();
        // Selection states
        static DWORD selectedVsPid = 0;
        static DWORD selectedCursorPid = 0;

        ImGui::BeginChild("vslist", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& inst : local)
        {
            char selLabel[64];
            snprintf(selLabel, sizeof(selLabel), "VS PID: %lu", (unsigned long)inst.pid);
            bool selected = (selectedVsPid == inst.pid);
            if (ImGui::Selectable(selLabel, selected)) selectedVsPid = inst.pid;
            if (!inst.windowTitle.empty())
                ImGui::TextDisabled("Title: %s", inst.windowTitle.c_str());
            if (!inst.exePath.empty())
                ImGui::TextDisabled("Path: %s", inst.exePath.c_str());
            if (!inst.solutionPath.empty())
            {
                ImGui::TextDisabled("Solution: %s", inst.solutionPath.c_str());
                bool checked = (g_selectedSlnPath == inst.solutionPath);
                std::string chkId = std::string("Use this solution##") + std::to_string((unsigned long)inst.pid);
                if (ImGui::Checkbox(chkId.c_str(), &checked))
                {
                    g_selectedSlnPath = checked ? inst.solutionPath : std::string();
                }
            }
            if (!inst.activeDocumentPath.empty())
                ImGui::TextDisabled("ActiveDocument: %s", inst.activeDocumentPath.c_str());
            ImGui::Separator();
        }
        // Append Cursor entries in the same list (peer to VS)
        for (const auto& c : localCursor)
        {
            if (c.folderPath.empty()) continue; // skip cursors without resolved folder
            char selLabel[64];
            snprintf(selLabel, sizeof(selLabel), "Cursor PID: %lu", (unsigned long)c.pid);
            bool selected = (selectedCursorPid == c.pid);
            if (ImGui::Selectable(selLabel, selected)) selectedCursorPid = c.pid;
            if (!c.windowTitle.empty())
                ImGui::TextDisabled("Title: %s", c.windowTitle.c_str());
            if (!c.exePath.empty())
                ImGui::TextDisabled("Path: %s", c.exePath.c_str());
            {
                ImGui::TextDisabled("Folder: %s", c.folderPath.c_str());
                bool checked = (g_selectedCursorFolder == c.folderPath);
                std::string chkId = std::string("Use this folder##") + std::to_string((unsigned long)c.pid);
                if (ImGui::Checkbox(chkId.c_str(), &checked))
                {
                    g_selectedCursorFolder = checked ? c.folderPath : std::string();
                }
            }
            ImGui::Separator();
        }
        ImGui::EndChild();

        // Current selections display
        ImGui::Separator();
        ImGui::Text("Current Saved Selections:");
        if (!g_selectedSlnPath.empty())
        {
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "VS Solution: %s", g_selectedSlnPath.c_str());
        }
        else
        {
            ImGui::TextDisabled("VS Solution: (none)");
        }
        if (!g_selectedCursorFolder.empty())
        {
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Cursor Folder: %s", g_selectedCursorFolder.c_str());
        }
        else
        {
            ImGui::TextDisabled("Cursor Folder: (none)");
        }
        
        ImGui::Separator();
        
        // Persist controls
        if (ImGui::Button("Save selection"))
        {
            SavePrefs();
        }
        ImGui::SameLine();
        if (ImGui::Button("Load selection"))
        {
            LoadPrefs();
        }
        
        ImGui::Separator();
        
        // Launch controls
        ImGui::Text("Quick Launch:");
        if (!g_selectedSlnPath.empty())
        {
            ImGui::SameLine();
            if (ImGui::Button("Launch VS"))
            {
                LaunchVSWithSolution(g_selectedSlnPath);
            }
        }
        if (!g_selectedCursorFolder.empty())
        {
            ImGui::SameLine();
            if (ImGui::Button("Launch Cursor"))
            {
                LaunchCursorWithFolder(g_selectedCursorFolder);
            }
        }
        if (g_selectedSlnPath.empty() && g_selectedCursorFolder.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(No selection saved)");
        }

        if (ImGui::CollapsingHeader("Log (from AppendLog)", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ReplaceTool::DrawSharedLog("vslog", 150.0f);
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
