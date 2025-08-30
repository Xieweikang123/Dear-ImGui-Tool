#include "imgui.h"
#include "src/replace_tool.h"
#include "src/vs_inspector.h"
#include "src/word_reminder.h"
#include "src/feature_manager.h"
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
#include <tlhelp32.h>
#include <unordered_map>
#include <objbase.h>
#include <oleauto.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#pragma comment(lib, "Shcore.lib")
#endif

namespace fs = std::filesystem;

// Shared state for the simple string replacement tool UI
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

static void AppendLog(const std::string& line)
{
    std::lock_guard<std::mutex> lock(g_state.logMutex);
    g_state.logLines.emplace_back(line);
    if (g_state.logFile.is_open())
    {
        g_state.logFile << line << '\n';
        g_state.logFile.flush();
    }
#ifdef _WIN32
    // Also mirror logs to a shared file used elsewhere by the app
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
#else
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
#endif
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

    // Prepare log file if enabled
    bool logOpened = false;
    if (g_state.writeLogToFile)
    {
        const std::string ts = MakeTimestamp();
        fs::path logPath = root / (std::string("replace_log_") + ts + ".txt");
        std::error_code lec;
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
        // Rename files first
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
        // Rename directories deepest-first
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

static void DrawUI()
{
    // 绘制主菜单栏
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("Tools"))
        {
            if (ImGui::MenuItem("Feature Manager"))
            {
                FeatureManager::GetInstance().ShowFeatureSelector();
            }
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("日志"))
        {
            if (ImGui::MenuItem("打开日志文件"))
            {
#ifdef _WIN32
                // 使用系统默认程序打开日志文件
                ShellExecuteA(NULL, "open", "DearImGuiExample.log", NULL, NULL, SW_SHOWNORMAL);
#else
                // 非Windows平台，尝试使用系统命令打开
                system("start DearImGuiExample.log");
#endif
            }
            ImGui::EndMenu();
        }
        
        ImGui::EndMainMenuBar();
    }
    
    // 先绘制其他功能窗口
    FeatureManager::GetInstance().DrawAllFeatures();
    
    // 最后绘制Feature Manager窗口（如果可见的话），确保它显示在最前面
    FeatureManager::GetInstance().DrawFeatureSelector();
}

static void DrawVSUI()
{
    // This function is now handled by the feature manager
    // Keeping it for backward compatibility
}

#ifdef IMGUI_USE_D3D11
// Win32 + DirectX11 backend
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <d3d11.h>
#include <tchar.h>
#include <cwchar>
#include <cstdio>

// Data
static ID3D11Device*               g_pd3dDevice = NULL;
static ID3D11DeviceContext*        g_pd3dDeviceContext = NULL;
static IDXGISwapChain*             g_pSwapChain = NULL;
static ID3D11RenderTargetView*     g_mainRenderTargetView = NULL;

static void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_mainRenderTargetView);
    if (pBackBuffer) pBackBuffer->Release();
}

static void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = NULL; }
}

static bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
#ifdef _DEBUG
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    if (D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, createDeviceFlags, featureLevelArray, 2,
        D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext) != S_OK)
        return false;

    // Log backend and adapter info
    {
        FILE* log = fopen("DearImGuiExample.log", "w");
        if (log)
        {
            fprintf(log, "Backend: Direct3D11\n");
            fprintf(log, "FeatureLevel: 0x%04x\n", (unsigned)featureLevel);
            IDXGIDevice* pDXGIDevice = NULL;
            if (SUCCEEDED(g_pd3dDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&pDXGIDevice)) && pDXGIDevice)
            {
                IDXGIAdapter* pAdapter = NULL;
                if (SUCCEEDED(pDXGIDevice->GetAdapter(&pAdapter)) && pAdapter)
                {
                    DXGI_ADAPTER_DESC desc;
                    if (SUCCEEDED(pAdapter->GetDesc(&desc)))
                    {
                        char name[256] = {0};
                        size_t conv = 0;
                        wcstombs_s(&conv, name, desc.Description, _TRUNCATE);
                        fprintf(log, "Adapter: %s\n", name);
                    }
                    pAdapter->Release();
                }
                pDXGIDevice->Release();
            }
            fclose(log);
        }
    }

    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = NULL; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = NULL; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = NULL; }
}

static void ResizeSwapChain(HWND hWnd)
{
    if (g_pd3dDevice != NULL)
    {
        CleanupRenderTarget();
        g_pSwapChain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
        CreateRenderTarget();
    }
}

// Win32 message handler
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
// ---- Tray icon support ----
static UINT WM_TRAYICON = WM_APP + 1;
static const UINT TRAY_ICON_ID = 1;
static const UINT ID_TRAY_SHOW = 10001;
static const UINT ID_TRAY_EXIT = 10002;
static NOTIFYICONDATA nid = {};
// ---- App icon (procedural) ----
static HICON g_appIcon = nullptr;
static HICON CreateAppIcon()
{
#ifdef _WIN32
    const int size = 32;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(BITMAPV5HEADER);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask   = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask  = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC hdc = GetDC(NULL);
    HBITMAP color = CreateDIBSection(hdc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, hdc);
    if (!color) return NULL;
    unsigned int* px = (unsigned int*)bits;
    for (int i = 0; i < size * size; ++i) px[i] = 0x00000000;
    auto setpx = [&](int x, int y, unsigned int argb){ if (x>=0 && x<size && y>=0 && y<size) px[y*size + x] = argb; };
    const int r = 6;
    for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x)
    {
        bool in = true;
        if (x < r && y < r) in = (x-r)*(x-r) + (y-r)*(y-r) <= r*r;
        else if (x >= size-r && y < r) in = (x-(size-1-r))*(x-(size-1-r)) + (y-r)*(y-r) <= r*r;
        else if (x < r && y >= size-r) in = (x-r)*(x-r) + (y-(size-1-r))*(y-(size-1-r)) <= r*r;
        else if (x >= size-r && y >= size-r) in = (x-(size-1-r))*(x-(size-1-r)) + (y-(size-1-r))*(y-(size-1-r)) <= r*r;
        if (in)
        {
            unsigned char a = 255;
            unsigned char g = (unsigned char)(140 + (y * 60 / size));
            unsigned char b = 230;
            unsigned char rr = 45;
            unsigned int argb = (a<<24) | (rr<<16) | (g<<8) | (b);
            setpx(x,y,argb);
        }
    }
    for (int y = 8; y <= 24; ++y) for (int x = 10; x <= 13; ++x) setpx(x,y,0xFFFFFFFF);
    for (int y = 8; y <= 24; ++y) for (int x = 19; x <= 22; ++x) setpx(x,y,0xFFFFFFFF);
    for (int x = 13; x <= 19; ++x) { setpx(x,8,0xFFFFFFFF); setpx(x,24,0xFFFFFFFF);} 
    HBITMAP mask = CreateBitmap(size, size, 1, 1, NULL);
    ICONINFO ii{}; ii.fIcon = TRUE; ii.hbmColor = color; ii.hbmMask = mask;
    HICON h = CreateIconIndirect(&ii);
    DeleteObject(color); DeleteObject(mask);
    return h;
#else
    return nullptr;
#endif
}

// Improve Windows DPI awareness to avoid blurry text on scaled displays
static void EnablePerMonitorDpiAwareness()
{
#ifdef _WIN32
    // Try modern per-monitor v2 DPI awareness if available
    HMODULE user32 = LoadLibraryW(L"user32.dll");
    if (user32)
    {
        using SetDpiCtx = BOOL (WINAPI*)(DPI_AWARENESS_CONTEXT);
        auto pSetAwarenessCtx = (SetDpiCtx)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (pSetAwarenessCtx)
        {
            pSetAwarenessCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            FreeLibrary(user32);
            return;
        }
        FreeLibrary(user32);
    }
    // Fallback to system DPI aware
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore)
    {
        typedef HRESULT (WINAPI *SetProcessDpiAwarenessFn)(PROCESS_DPI_AWARENESS);
        auto pSetProcessDpiAwareness = (SetProcessDpiAwarenessFn)GetProcAddress(shcore, "SetProcessDpiAwareness");
        if (pSetProcessDpiAwareness)
            pSetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
        FreeLibrary(shcore);
    }
#endif
}

static float GetDpiScaleForWindow(HWND hwnd)
{
#ifdef _WIN32
    // Prefer GetDpiForWindow (Win10+)
    HMODULE user32 = LoadLibraryW(L"user32.dll");
    if (user32)
    {
        typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
        auto pGetDpiForWindow = (GetDpiForWindowFn)GetProcAddress(user32, "GetDpiForWindow");
        if (pGetDpiForWindow)
        {
            UINT dpi = pGetDpiForWindow(hwnd);
            FreeLibrary(user32);
            return dpi > 0 ? (float)dpi / 96.0f : 1.0f;
        }
        FreeLibrary(user32);
    }
    // Fallback: per-monitor DPI via Shcore
    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    UINT dpiX = 96, dpiY = 96;
    if (GetDpiForMonitor)
    {
        GetDpiForMonitor(hMon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    }
    return (float)dpiX / 96.0f;
#else
    return 1.0f;
#endif
}

static void AddTrayIcon(HWND hWnd)
{
    if (nid.cbSize != 0) return;
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = hWnd;
    nid.uID = TRAY_ICON_ID;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = g_appIcon ? g_appIcon : LoadIcon(NULL, IDI_APPLICATION);
    lstrcpyn(nid.szTip, TEXT("单词学习提醒"), ARRAYSIZE(nid.szTip));
    Shell_NotifyIcon(NIM_ADD, &nid);
}

static void RemoveTrayIcon()
{
    if (nid.cbSize == 0) return;
    Shell_NotifyIcon(NIM_DELETE, &nid);
    ZeroMemory(&nid, sizeof(nid));
}

static void ShowTrayMenu(HWND hWnd)
{
    POINT pt; GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenu(menu, MF_STRING, ID_TRAY_SHOW, TEXT("显示窗口"));
    AppendMenu(menu, MF_STRING, ID_TRAY_EXIT, TEXT("退出"));
    SetForegroundWindow(hWnd);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_BOTTOMALIGN | TPM_RIGHTALIGN, pt.x, pt.y, 0, hWnd, NULL);
    DestroyMenu(menu);
    if (cmd == ID_TRAY_SHOW)
    {
        ShowWindow(hWnd, SW_SHOW);
        SetForegroundWindow(hWnd);
    }
    else if (cmd == ID_TRAY_EXIT)
    {
        // 在真正退出前保存窗口大小
        RECT rect;
        if (GetWindowRect(hWnd, &rect)) {
            int width = rect.right - rect.left;
            int height = rect.bottom - rect.top;
            int x = rect.left;
            int y = rect.top;
            
            AppendLog("[window] Exiting application, current size: " + std::to_string(width) + "x" + std::to_string(height) + " at " + std::to_string(x) + "," + std::to_string(y));
            
            if (width > 0 && height > 0 && width < 10000 && height < 10000) {
                std::ofstream configFile("window_config.txt");
                if (configFile.is_open()) {
                    configFile << width << " " << height << " " << x << " " << y;
                    configFile.close();
                    AppendLog("[window] Window config saved to window_config.txt (exit)");
                } else {
                    AppendLog("[window] Failed to save window config (exit)");
                }
            } else {
                AppendLog("[window] Invalid window size detected, skipping save (exit): " + std::to_string(width) + "x" + std::to_string(height));
            }
        } else {
            AppendLog("[window] Failed to get window rect, skipping save (exit)");
        }
        
        RemoveTrayIcon();
        PostMessage(hWnd, WM_CLOSE, 0, 0);
    }
}
static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
            ResizeSwapChain(hWnd);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            ShowWindow(hWnd, SW_HIDE);
            AddTrayIcon(hWnd);
            return 0;
        }
        break;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        if ((wParam & 0xfff0) == SC_CLOSE)
        {
            // 在隐藏窗口前保存窗口大小
            RECT rect;
            if (GetWindowRect(hWnd, &rect)) {
                int width = rect.right - rect.left;
                int height = rect.bottom - rect.top;
                int x = rect.left;
                int y = rect.top;
                
                AppendLog("[window] Closing window, current size: " + std::to_string(width) + "x" + std::to_string(height) + " at " + std::to_string(x) + "," + std::to_string(y));
                
                if (width > 0 && height > 0 && width < 10000 && height < 10000) {
                    std::ofstream configFile("window_config.txt");
                    if (configFile.is_open()) {
                        configFile << width << " " << height << " " << x << " " << y;
                        configFile.close();
                        AppendLog("[window] Window config saved to window_config.txt");
                    } else {
                        AppendLog("[window] Failed to save window config");
                    }
                } else {
                    AppendLog("[window] Invalid window size detected, skipping save: " + std::to_string(width) + "x" + std::to_string(height));
                }
            } else {
                AppendLog("[window] Failed to get window rect, skipping save");
            }
            
            ShowWindow(hWnd, SW_HIDE);
            AddTrayIcon(hWnd);
            return 0;
        }
        break;
    case WM_APP+1: // WM_TRAYICON
        if (lParam == WM_LBUTTONUP)
        {
            ShowWindow(hWnd, SW_SHOW);
            SetForegroundWindow(hWnd);
            return 0;
        }
        else if (lParam == WM_RBUTTONUP)
        {
            ShowTrayMenu(hWnd);
            return 0;
        }
        break;
    case WM_DESTROY:
        RemoveTrayIcon();
        if (g_appIcon) { DestroyIcon(g_appIcon); g_appIcon = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    // Create Win32 window
    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, _T("ImGui Example"), NULL };
    RegisterClassEx(&wc);
    EnablePerMonitorDpiAwareness();
    g_appIcon = CreateAppIcon();
    HWND hwnd = CreateWindow(wc.lpszClassName, _T("Dear ImGui Minimal Example (D3D11)"), WS_OVERLAPPEDWINDOW, 100, 100, 1280, 720, NULL, NULL, wc.hInstance, NULL);
    if (g_appIcon && hwnd)
    {
        SendMessage(hwnd, WM_SETICON, ICON_BIG,   (LPARAM)g_appIcon);
        SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g_appIcon);
    }

    // Initialize D3D
    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        UnregisterClass(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);
    
    // 记录当前窗口大小
    RECT rect;
    GetWindowRect(hwnd, &rect);
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    AppendLog("[window] Initial window size: " + std::to_string(width) + "x" + std::to_string(height));
    
    // 尝试从配置文件恢复窗口大小
    std::ifstream winConfigFile("window_config.txt");
    if (winConfigFile.is_open()) {
        int savedWidth, savedHeight, savedX, savedY;
        if (winConfigFile >> savedWidth >> savedHeight >> savedX >> savedY) {
            // 检查窗口位置是否有效（不在屏幕外）
            if (savedX >= -10000 && savedY >= -10000 && savedX < 10000 && savedY < 10000 && 
                savedWidth > 100 && savedHeight > 100 && savedWidth < 5000 && savedHeight < 5000) {
                AppendLog("[window] Restoring window size: " + std::to_string(savedWidth) + "x" + std::to_string(savedHeight) + " at " + std::to_string(savedX) + "," + std::to_string(savedY));
                SetWindowPos(hwnd, NULL, savedX, savedY, savedWidth, savedHeight, SWP_NOZORDER);
            } else {
                AppendLog("[window] Invalid window position detected, using default size: " + std::to_string(savedWidth) + "x" + std::to_string(savedHeight) + " at " + std::to_string(savedX) + "," + std::to_string(savedY));
                // 使用默认位置和大小
                SetWindowPos(hwnd, NULL, 100, 100, 1280, 720, SWP_NOZORDER);
            }
        }
        winConfigFile.close();
    } else {
        AppendLog("[window] No saved window config found, using default size");
    }
    
    // Keep tray icon persistent regardless of window visibility
    AddTrayIcon(hwnd);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    // 启用窗口大小和位置保存功能
    io.IniFilename = "imgui.ini";
    AppendLog("[window] Setting IniFilename to: imgui.ini");
    
    // 检查imgui.ini文件是否存在
    if (fs::exists("imgui.ini")) {
        AppendLog("[window] imgui.ini file exists");
        std::ifstream iniFile("imgui.ini");
        std::string line;
        while (std::getline(iniFile, line)) {
            if (line.find("[Window]") != std::string::npos) {
                AppendLog("[window] Found window config: " + line);
            }
        }
        iniFile.close();
    } else {
        AppendLog("[window] imgui.ini file does not exist");
    }
    
    ImGui::StyleColorsDark();
    const float dpiScale = GetDpiScaleForWindow(hwnd);
    ImGui::GetStyle().ScaleAllSizes(dpiScale);

    // Setup Platform/Renderer bindings
    ImGui_ImplWin32_Init(hwnd);
    // Enable IME in Win32 backend explicitly
    ImGui::GetIO().WantCaptureKeyboard = true;
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // Initialize feature manager and all features
    FeatureManager::GetInstance().Initialize();

    // Font loading and verbose logging (D3D11 path)
    {
        AppendLog("[font] D3D11: starting font setup");
        ImGuiIO& io = ImGui::GetIO();
        ImFont* defaultFont = io.Fonts->AddFontDefault();
        if (defaultFont) AppendLog("[font] D3D11: added default font");

        ImFont* chineseFont = nullptr;
        const char* localCandidates[] = {
            "fonts/NotoSansSC-Regular.otf",
            "fonts/NotoSansSC-Regular.ttf",
            "fonts/SourceHanSansCN-Regular.otf",
            "fonts/SourceHanSansCN-Regular.ttf",
            "fonts/MSYH.TTC",
            "fonts/msyh.ttc",
            "fonts/SIMSUN.TTC",
            "fonts/simsun.ttc"
        };

        // Build search dirs: CWD fonts/Fonts and EXE_DIR fonts/Fonts
        std::vector<fs::path> fontSearchDirs;
        fontSearchDirs.push_back(fs::path("fonts"));
        fontSearchDirs.push_back(fs::path("Fonts"));
        wchar_t exePathW[MAX_PATH] = {0};
        if (GetModuleFileNameW(NULL, exePathW, MAX_PATH) > 0)
        {
            fs::path exeDir = fs::path(exePathW).parent_path();
            fontSearchDirs.push_back(exeDir / "fonts");
            fontSearchDirs.push_back(exeDir / "Fonts");
            AppendLog(std::string("[font] D3D11: exeDir= ") + exeDir.string());
        }
        AppendLog(std::string("[font] D3D11: current_path= ") + fs::current_path().string());

        for (const auto& dir : fontSearchDirs)
        {
            AppendLog(std::string("[font] D3D11: check dir: ") + dir.string() + (fs::exists(dir) ? " [exists]" : " [missing]"));
            if (!fs::exists(dir)) continue;
            for (const char* rel : localCandidates)
            {
                fs::path candidate = dir / fs::path(rel).filename();
                if (!fs::exists(candidate)) continue;
                std::string p = candidate.string();
                AppendLog(std::string("[font] D3D11: trying bundled font: ") + p);
                ImFontConfig cfg; cfg.OversampleH = 3; cfg.OversampleV = 1; cfg.RasterizerMultiply = 1.0f; cfg.PixelSnapH = true;
                chineseFont = io.Fonts->AddFontFromFileTTF(p.c_str(), 16.0f * dpiScale, &cfg, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
                if (chineseFont) { AppendLog(std::string("[font] Loaded Chinese font (bundled): ") + p); break; }
                else { AppendLog(std::string("[font] failed to load: ") + p); }
            }
            if (chineseFont) break;
            // Scan directory for any ttf/otf/ttc
            for (auto& entry : fs::directory_iterator(dir))
            {
                if (!entry.is_regular_file()) continue;
                auto ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".ttf" || ext == ".otf" || ext == ".ttc")
                {
                    std::string p = entry.path().string();
                    AppendLog(std::string("[font] D3D11: scanning try: ") + p);
                    ImFontConfig cfg; cfg.OversampleH = 3; cfg.OversampleV = 1; cfg.RasterizerMultiply = 1.0f; cfg.PixelSnapH = true;
                    chineseFont = io.Fonts->AddFontFromFileTTF(p.c_str(), 16.0f * dpiScale, &cfg, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
                    if (chineseFont) { AppendLog(std::string("[font] Loaded Chinese font (scanned): ") + p); break; }
                }
            }
            if (chineseFont) break;
        }

        // System fallback
        if (!chineseFont)
        {
            const char* sysFonts[] = {
                "C:/Windows/Fonts/msyh.ttc",
                "C:/Windows/Fonts/simsun.ttc",
                "C:/Windows/Fonts/msyh.ttf",
                "C:/Windows/Fonts/simsun.ttf"
            };
            for (const char* fp : sysFonts)
            {
                AppendLog(std::string("[font] D3D11: try system font: ") + fp);
                if (!fs::exists(fp)) { AppendLog("[font] D3D11: not found"); continue; }
                ImFontConfig cfg; cfg.OversampleH = 3; cfg.OversampleV = 1; cfg.RasterizerMultiply = 1.0f; cfg.PixelSnapH = true;
                chineseFont = io.Fonts->AddFontFromFileTTF(fp, 16.0f * dpiScale, &cfg, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
                if (chineseFont) { AppendLog(std::string("[font] Loaded Chinese font (system): ") + fp); break; }
            }
        }

        // Emoji fallback: load a monochrome-capable emoji font and MERGE into atlas
        ImFont* emojiFont = nullptr;
        const char* emojiBundled[] = {
            "fonts/NotoEmoji-Regular.ttf",      // monochrome glyphs -> works with stb
            "fonts/NotoColorEmoji.ttf"          // color requires FreeType; may not render with stb
        };
        const char* emojiSystem[] = {
            "C:/Windows/Fonts/seguisym.ttf",    // Segoe UI Symbol (monochrome)
            "C:/Windows/Fonts/seguiemj.ttf",    // Segoe UI Emoji (color; needs FreeType for color)
            "C:/Windows/Fonts/seguiemj.ttc"
        };

        // Build emoji ranges
        ImVector<ImWchar> emojiRanges; ImFontGlyphRangesBuilder builder; 
        builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
        // Common emoji blocks
        static const ImWchar range_arrows[]      = { 0x2190, 0x21FF, 0 };
        static const ImWchar range_symbols[]     = { 0x2600, 0x27FF, 0 };
        static const ImWchar range_misc_pict[]   = { 0x1F300, 0x1F6FF, 0 };
        static const ImWchar range_supp_pict[]   = { 0x1F900, 0x1F9FF, 0 };
        static const ImWchar range_pict_ext_a[]  = { 0x1FA70, 0x1FAFF, 0 };
        builder.AddRanges(range_arrows);
        builder.AddRanges(range_symbols);
        builder.AddRanges(range_misc_pict);
        builder.AddRanges(range_supp_pict);
        builder.AddRanges(range_pict_ext_a);
        builder.BuildRanges(&emojiRanges);

        ImFontConfig cfg; cfg.MergeMode = true; cfg.PixelSnapH = true; cfg.GlyphMinAdvanceX = 0.0f;
        // Try bundled first
        for (const auto& dir : fontSearchDirs)
        {
            if (emojiFont) break; if (!fs::exists(dir)) continue;
            for (const char* rel : emojiBundled)
            {
                fs::path p = dir / fs::path(rel).filename();
                if (!fs::exists(p)) continue;
                AppendLog(std::string("[font] D3D11: merge emoji bundled: ") + p.string());
                emojiFont = io.Fonts->AddFontFromFileTTF(p.string().c_str(), 16.0f * dpiScale, &cfg, emojiRanges.Data);
                if (emojiFont) { AppendLog(std::string("[font] Merged emoji font (bundled): ") + p.string()); break; }
            }
        }
        // Then system
        if (!emojiFont)
        {
            for (const char* fp : emojiSystem)
            {
                if (!fs::exists(fp)) continue;
                AppendLog(std::string("[font] D3D11: merge emoji system: ") + fp);
                emojiFont = io.Fonts->AddFontFromFileTTF(fp, 16.0f * dpiScale, &cfg, emojiRanges.Data);
                if (emojiFont) { AppendLog(std::string("[font] Merged emoji font (system): ") + fp); break; }
            }
        }

        // Set default font preference
        if (chineseFont)
        {
            io.FontDefault = chineseFont;
            AppendLog("[font] D3D11: Using Chinese font as default");
        }
        else if (emojiFont)
        {
            io.FontDefault = emojiFont;
            AppendLog("[font] D3D11: Using emoji font as default");
        }
        else
        {
            io.FontDefault = defaultFont;
            AppendLog("[font] D3D11: Using default font");
        }
    }

    // Main loop
    bool done = false;
    while (!done)
    {
        MSG msg;
        while (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        DrawUI();
        DrawVSUI();
        
        // 检查单词提醒
        if (WordReminder::HasReminderToShow())
        {
            // 这里可以添加声音提醒或其他提醒方式
            // 目前会在UI中显示弹窗
        }

        ImGui::Render();
        const float clear_color_with_alpha[4] = { 0.45f, 0.55f, 0.60f, 1.00f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    // 窗口大小已经在隐藏时保存，这里不需要重复保存
    
    // Cleanup
    FeatureManager::GetInstance().Cleanup();
    
    // 保存ImGui设置到ini文件
    AppendLog("[window] Saving ImGui settings to imgui.ini");
    ImGui::SaveIniSettingsToDisk("imgui.ini");
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClass(wc.lpszClassName, wc.hInstance);

    return 0;
}

#else
// GLFW + OpenGL backend (non-Windows or when D3D11 disabled)
#include "imgui_impl_glfw.h"
#ifdef IMGUI_USE_OPENGL2
#include "imgui_impl_opengl2.h"
#else
#include "imgui_impl_opengl3.h"
#endif
#include <stdio.h>
#include <GLFW/glfw3.h>

static void glfw_error_callback(int error, const char* description)
{
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

int main(int, char**)
{

    //log
    AppendLog("main");

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return 1;
#ifndef IMGUI_USE_OPENGL2
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#else
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_FALSE);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_ANY_PROFILE);
#endif

    GLFWwindow* window = glfwCreateWindow(1280, 720, "Dear ImGui Minimal Example", nullptr, nullptr);
    if (window == nullptr)
        return 1;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    
    // 记录当前窗口大小
    int width, height;
    glfwGetWindowSize(window, &width, &height);
    AppendLog("[window] Initial GLFW window size: " + std::to_string(width) + "x" + std::to_string(height));
    
    // 尝试从配置文件恢复窗口大小
    std::ifstream glfwConfigFile("window_config.txt");
    if (glfwConfigFile.is_open()) {
        int savedWidth, savedHeight, savedX, savedY;
        if (glfwConfigFile >> savedWidth >> savedHeight >> savedX >> savedY) {
            // 检查窗口位置是否有效（不在屏幕外）
            if (savedX >= -10000 && savedY >= -10000 && savedX < 10000 && savedY < 10000 && 
                savedWidth > 100 && savedHeight > 100 && savedWidth < 5000 && savedHeight < 5000) {
                AppendLog("[window] Restoring GLFW window size: " + std::to_string(savedWidth) + "x" + std::to_string(savedHeight) + " at " + std::to_string(savedX) + "," + std::to_string(savedY));
                glfwSetWindowSize(window, savedWidth, savedHeight);
                glfwSetWindowPos(window, savedX, savedY);
            } else {
                AppendLog("[window] Invalid GLFW window position detected, using default size: " + std::to_string(savedWidth) + "x" + std::to_string(savedHeight) + " at " + std::to_string(savedX) + "," + std::to_string(savedY));
                // 使用默认位置和大小
                glfwSetWindowSize(window, 1280, 720);
                glfwSetWindowPos(window, 100, 100);
            }
        }
        glfwConfigFile.close();
    } else {
        AppendLog("[window] No saved GLFW window config found, using default size");
    }

    // Log backend and GL info
    {
        FILE* log = fopen("DearImGuiExample.log", "w");
        if (log)
        {
            const unsigned char* renderer = glGetString(GL_RENDERER);
            const unsigned char* version = glGetString(GL_VERSION);
            fprintf(log, "Backend: OpenGL%s\n", 
#ifdef IMGUI_USE_OPENGL2
                "2"
#else
                "3"
#endif
            );
            fprintf(log, "GL Renderer: %s\n", renderer ? (const char*)renderer : "<null>");
            fprintf(log, "GL Version: %s\n", version ? (const char*)version : "<null>");
            fclose(log);
        }
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    // 启用窗口大小和位置保存功能
    io.IniFilename = "imgui.ini";
    AppendLog("[window] Setting IniFilename to: imgui.ini (GLFW)");
    
    // 检查imgui.ini文件是否存在
    if (fs::exists("imgui.ini")) {
        AppendLog("[window] imgui.ini file exists (GLFW)");
        std::ifstream iniFile("imgui.ini");
        std::string line;
        while (std::getline(iniFile, line)) {
            if (line.find("[Window]") != std::string::npos) {
                AppendLog("[window] Found window config (GLFW): " + line);
            }
        }
        iniFile.close();
    } else {
        AppendLog("[window] imgui.ini file does not exist (GLFW)");
    }
    
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
#ifdef IMGUI_USE_OPENGL2
    ImGui_ImplOpenGL2_Init();
#else
    ImGui_ImplOpenGL3_Init(glsl_version);
#endif

    // Initialize feature manager and all features
    FeatureManager::GetInstance().Initialize();

    // 改进的字体加载策略
    ImFont* defaultFont = io.Fonts->AddFontDefault();
    
    // 尝试加载支持中文和emoji的字体（优先从本地 fonts/ 目录加载，便于自带字体）
    ImFont* chineseFont = nullptr;
    {
        // 首选预置候选（若随程序打包）
        const char* localCandidates[] = {
            "fonts/NotoSansSC-Regular.otf",
            "fonts/NotoSansSC-Regular.ttf",
            "fonts/SourceHanSansCN-Regular.otf",
            "fonts/SourceHanSansCN-Regular.ttf",
            "fonts/MSYH.TTC",
            "fonts/msyh.ttc",
            "fonts/SIMSUN.TTC",
            "fonts/simsun.ttc"
        };
        // 构建搜索目录：当前工作目录下 fonts/ 与 Fonts/，以及可执行目录下的 fonts/ 与 Fonts/
        std::vector<fs::path> fontSearchDirs;
        fontSearchDirs.push_back(fs::path("fonts"));
        fontSearchDirs.push_back(fs::path("Fonts"));
#ifdef _WIN32
        {
            wchar_t exePathW[MAX_PATH] = {0};
            if (GetModuleFileNameW(NULL, exePathW, MAX_PATH) > 0)
            {
                fs::path exeDir = fs::path(exePathW).parent_path();
                fontSearchDirs.push_back(exeDir / "fonts");
                fontSearchDirs.push_back(exeDir / "Fonts");
            }
        }
#endif
        for (const auto& dir : fontSearchDirs)
        {
            if (!fs::exists(dir)) continue;
            AppendLog(std::string("[font] Searching bundled fonts under: ") + dir.string());
            for (const char* rel : localCandidates)
            {
                fs::path candidate = dir / fs::path(rel).filename();
                if (!fs::exists(candidate)) continue;
                std::string p = candidate.string();
                chineseFont = io.Fonts->AddFontFromFileTTF(p.c_str(), 16.0f, nullptr, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
                if (chineseFont) { AppendLog(std::string("[font] Loaded Chinese font (bundled): ") + p); break; }
            }
            if (chineseFont) break;
        }
        // 若以上未命中，则扫描 fonts 目录的所有 ttf/otf
        if (!chineseFont)
        {
            for (const auto& dir : fontSearchDirs)
            {
                if (!fs::exists(dir)) continue;
                AppendLog(std::string("[font] scanning fonts directory: ") + dir.string());
                for (auto& entry : fs::directory_iterator(dir))
                {
                    if (!entry.is_regular_file()) continue;
                    auto ext = entry.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext == ".ttf" || ext == ".otf" || ext == ".ttc")
                    {
                        std::string p = entry.path().string();
                        chineseFont = io.Fonts->AddFontFromFileTTF(p.c_str(), 16.0f, nullptr, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
                        if (chineseFont) { AppendLog(std::string("[font] Loaded Chinese font (scanned): ") + p); break; }
                    }
                }
                if (chineseFont) break;
            }
        }
        // 系统字体兜底
        if (!chineseFont)
        {
            const char* chineseFontPaths[] = {
                "C:/Windows/Fonts/msyh.ttc",
                "C:/Windows/Fonts/simsun.ttc",
                "C:/Windows/Fonts/msyh.ttf",
                "C:/Windows/Fonts/simsun.ttf"
            };
            for (const char* fontPath : chineseFontPaths)
            {
                if (!fs::exists(fontPath)) continue;
                chineseFont = io.Fonts->AddFontFromFileTTF(fontPath, 16.0f, nullptr, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
                if (chineseFont) { AppendLog(std::string("[font] Loaded Chinese font (system): ") + fontPath); break; }
            }
        }
    }
    
    // 尝试加载emoji字体作为补充
    ImFont* emojiFont = nullptr;
    const char* emojiFontPaths[] = {
        "fonts/NotoColorEmoji.ttf",
        "fonts/NotoEmoji-Regular.ttf",
        "C:/Windows/Fonts/seguiemj.ttf",
        "C:/Windows/Fonts/seguiemj.ttc",
        "C:/Windows/Fonts/arial.ttf"
    };
    
    for (const char* fontPath : emojiFontPaths)
    {
        emojiFont = io.Fonts->AddFontFromFileTTF(fontPath, 16.0f, nullptr, io.Fonts->GetGlyphRangesDefault());
        if (emojiFont) 
        {
            AppendLog(std::string("[font] Loaded emoji font: ") + fontPath);
            break;
        }
    }
    
    // 设置字体优先级：中文字体 > emoji字体 > 默认字体
    if (chineseFont)
    {
        io.FontDefault = chineseFont;
        AppendLog("[font] Using Chinese font as default");
    }
    else if (emojiFont)
    {
        io.FontDefault = emojiFont;
        AppendLog("[font] Using emoji font as default");
    }
    else
    {
        io.FontDefault = defaultFont;
        AppendLog("[font] Using default font");
    }

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();
#ifdef IMGUI_USE_OPENGL2
        ImGui_ImplOpenGL2_NewFrame();
#else
        ImGui_ImplOpenGL3_NewFrame();
#endif
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        DrawUI();
        DrawVSUI();
        
        // 检查单词提醒
        if (WordReminder::HasReminderToShow())
        {
            // 这里可以添加声音提醒或其他提醒方式
            // 目前会在UI中显示弹窗
        }

        ImGui::Render();
        int display_w, display_h; glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.45f, 0.55f, 0.60f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
#ifdef IMGUI_USE_OPENGL2
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
#else
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
#endif
        glfwSwapBuffers(window);
    }

    // 保存当前窗口大小和位置（在清理之前）
    int finalWidth, finalHeight;
    glfwGetWindowSize(window, &finalWidth, &finalHeight);
    int finalX, finalY;
    glfwGetWindowPos(window, &finalX, &finalY);
    
    // 验证窗口大小的合理性
    if (finalWidth > 0 && finalHeight > 0 && finalWidth < 10000 && finalHeight < 10000) {
        AppendLog("[window] Saving GLFW window size: " + std::to_string(finalWidth) + "x" + std::to_string(finalHeight) + " at " + std::to_string(finalX) + "," + std::to_string(finalY));
        
        std::ofstream glfwConfigFileOut("window_config.txt");
        if (glfwConfigFileOut.is_open()) {
            glfwConfigFileOut << finalWidth << " " << finalHeight << " " << finalX << " " << finalY;
            glfwConfigFileOut.close();
            AppendLog("[window] GLFW window config saved to window_config.txt");
        } else {
            AppendLog("[window] Failed to save GLFW window config");
        }
    } else {
        AppendLog("[window] Invalid GLFW window size detected, skipping save: " + std::to_string(finalWidth) + "x" + std::to_string(finalHeight));
    }
    
    FeatureManager::GetInstance().Cleanup();
    
    // 保存ImGui设置到ini文件
    AppendLog("[window] Saving ImGui settings to imgui.ini (GLFW)");
    ImGui::SaveIniSettingsToDisk("imgui.ini");
#ifdef IMGUI_USE_OPENGL2
    ImGui_ImplOpenGL2_Shutdown();
#else
    ImGui_ImplOpenGL3_Shutdown();
#endif
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
#endif
