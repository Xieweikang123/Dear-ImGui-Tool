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
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;
    case WM_DESTROY:
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
    HWND hwnd = CreateWindow(wc.lpszClassName, _T("Dear ImGui Minimal Example (D3D11)"), WS_OVERLAPPEDWINDOW, 100, 100, 1280, 720, NULL, NULL, wc.hInstance, NULL);

    // Initialize D3D
    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        UnregisterClass(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer bindings
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

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

        ImGui::Render();
        const float clear_color_with_alpha[4] = { 0.45f, 0.55f, 0.60f, 1.00f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    // Cleanup
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
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
#ifdef IMGUI_USE_OPENGL2
    ImGui_ImplOpenGL2_Init();
#else
    ImGui_ImplOpenGL3_Init(glsl_version);
#endif

    io.Fonts->AddFontDefault();

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
