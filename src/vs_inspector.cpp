#include "vs_inspector.h"
#include "replace_tool.h"

#include "imgui.h"
#include <string>
#include <vector>
#include <mutex>
#include <algorithm>
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

    static std::vector<VSInstance> g_vsList;
    static std::mutex g_vsMutexVS;

    static std::string WideToUtf8(const std::wstring& w)
    {
        if (w.empty()) return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

    void Refresh()
    {
        AppendLog("[vs] RefreshVSInstances: begin");
        std::vector<VSInstance> found;

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
                    found.push_back(inst);
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
        }

        HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        bool didCoInit = SUCCEEDED(hr);
        static bool comSecurityInitialized = false;
        if (didCoInit && !comSecurityInitialized)
        {
            HRESULT hrSec = CoInitializeSecurity(NULL, -1, NULL, NULL,
                RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
            if (SUCCEEDED(hrSec)) comSecurityInitialized = true;
        }
        IRunningObjectTable* pRot = NULL;
        IEnumMoniker* pEnum = NULL;
        if (SUCCEEDED(GetRunningObjectTable(0, &pRot)) && pRot)
        {
            if (SUCCEEDED(pRot->EnumRunning(&pEnum)) && pEnum)
            {
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
                            IUnknown* pUnk = NULL;
                            if (SUCCEEDED(pRot->GetObject(pMoniker, &pUnk)) && pUnk)
                            {
                                IDispatch* pDisp = NULL;
                                if (SUCCEEDED(pUnk->QueryInterface(IID_IDispatch, (void**)&pDisp)) && pDisp)
                                {
                                    DISPID dispidMainWindow = 0;
                                    OLECHAR* nameMainWindow = L"MainWindow";
                                    if (SUCCEEDED(pDisp->GetIDsOfNames(IID_NULL, &nameMainWindow, 1, LOCALE_USER_DEFAULT, &dispidMainWindow)))
                                    {
                                        VARIANT resultMainWindow; VariantInit(&resultMainWindow);
                                        DISPPARAMS noArgs = {0};
                                        if (SUCCEEDED(pDisp->Invoke(dispidMainWindow, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &noArgs, &resultMainWindow, NULL, NULL)))
                                        {
                                            if (resultMainWindow.vt == VT_DISPATCH && resultMainWindow.pdispVal)
                                            {
                                                IDispatch* pMainWin = resultMainWindow.pdispVal;
                                                DISPID dispidHWnd = 0;
                                                OLECHAR* nameHWnd = L"HWnd";
                                                if (SUCCEEDED(pMainWin->GetIDsOfNames(IID_NULL, &nameHWnd, 1, LOCALE_USER_DEFAULT, &dispidHWnd)))
                                                {
                                                    VARIANT resultHwnd; VariantInit(&resultHwnd);
                                                    if (SUCCEEDED(pMainWin->Invoke(dispidHWnd, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &noArgs, &resultHwnd, NULL, NULL)))
                                                    {
                                                        LONG hwndVal = 0;
                                                        if (resultHwnd.vt == VT_I4) hwndVal = resultHwnd.lVal;
                                                        else if (resultHwnd.vt == VT_I2) hwndVal = resultHwnd.iVal;
                                                        if (hwndVal)
                                                        {
                                                            DWORD pid = 0;
                                                            GetWindowThreadProcessId((HWND)(INT_PTR)hwndVal, &pid);
                                                            if (pid)
                                                            {
                                                                DISPID dispidSolution = 0;
                                                                OLECHAR* nameSolution = L"Solution";
                                                                if (SUCCEEDED(pDisp->GetIDsOfNames(IID_NULL, &nameSolution, 1, LOCALE_USER_DEFAULT, &dispidSolution)))
                                                                {
                                                                    VARIANT resultSolution; VariantInit(&resultSolution);
                                                                    if (SUCCEEDED(pDisp->Invoke(dispidSolution, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &noArgs, &resultSolution, NULL, NULL)))
                                                                    {
                                                                        std::string sln;
                                                                        if (resultSolution.vt == VT_DISPATCH && resultSolution.pdispVal)
                                                                        {
                                                                            IDispatch* pSolution = resultSolution.pdispVal;
                                                                            DISPID dispidFullName = 0;
                                                                            OLECHAR* nameFullName = L"FullName";
                                                                            if (SUCCEEDED(pSolution->GetIDsOfNames(IID_NULL, &nameFullName, 1, LOCALE_USER_DEFAULT, &dispidFullName)))
                                                                            {
                                                                                VARIANT resultFullName; VariantInit(&resultFullName);
                                                                                if (SUCCEEDED(pSolution->Invoke(dispidFullName, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &noArgs, &resultFullName, NULL, NULL)))
                                                                                {
                                                                                    if (resultFullName.vt == VT_BSTR && resultFullName.bstrVal)
                                                                                    {
                                                                                        sln = WideToUtf8(resultFullName.bstrVal);
                                                                                    }
                                                                                    VariantClear(&resultFullName);
                                                                                }
                                                                            }
                                                                        }
                                                                        if (sln.empty())
                                                                        {
                                                                            DISPID dispidActiveDoc = 0;
                                                                            OLECHAR* nameActiveDoc = L"ActiveDocument";
                                                                            if (SUCCEEDED(pDisp->GetIDsOfNames(IID_NULL, &nameActiveDoc, 1, LOCALE_USER_DEFAULT, &dispidActiveDoc)))
                                                                            {
                                                                                VARIANT resultActiveDoc; VariantInit(&resultActiveDoc);
                                                                                if (SUCCEEDED(pDisp->Invoke(dispidActiveDoc, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &noArgs, &resultActiveDoc, NULL, NULL)))
                                                                                {
                                                                                    ReplaceTool::AppendLog("ActiveDocument");
                                                                                    if (resultActiveDoc.vt == VT_DISPATCH && resultActiveDoc.pdispVal)
                                                                                    {
                                                                                        IDispatch* pDoc = resultActiveDoc.pdispVal;
                                                                                        DISPID dispidDocFullName = 0;
                                                                                        OLECHAR* nameDocFullName = L"FullName";
                                                                                        if (SUCCEEDED(pDoc->GetIDsOfNames(IID_NULL, &nameDocFullName, 1, LOCALE_USER_DEFAULT, &dispidDocFullName)))
                                                                                        {
                                                                                            VARIANT resultDocFN; VariantInit(&resultDocFN);
                                                                                            ReplaceTool::AppendLog("FullName");
                                                                                            if (SUCCEEDED(pDoc->Invoke(dispidDocFullName, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &noArgs, &resultDocFN, NULL, NULL)))
                                                                                            {
                                                                                                if (resultDocFN.vt == VT_BSTR && resultDocFN.bstrVal)
                                                                                                {
                                                                                                    std::string docPath = WideToUtf8(resultDocFN.bstrVal);
                                                                                                    ReplaceTool::AppendLog(std::string("[vs] pid ") + std::to_string((unsigned long)pid) + std::string(" ActiveDocument: ") + docPath);
                                                                                                    for (auto& inst : found)
                                                                                                    {
                                                                                                        if (inst.pid == pid)
                                                                                                        {
                                                                                                            inst.activeDocumentPath = docPath;
                                                                                                        }
                                                                                                    }
                                                                                                    std::error_code ec2;
                                                                                                    fs::path pdir = fs::path(docPath).parent_path();
                                                                                                    int depth = 0;
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
                                                                                                                    sln = e.path().string();
                                                                                                                    ReplaceTool::AppendLog(std::string("[vs] pid ") + std::to_string((unsigned long)pid) + std::string(" Found nearby solution: ") + sln);
                                                                                                                    break;
                                                                                                                }
                                                                                                            }
                                                                                                        }
                                                                                                        if (!sln.empty()) break;
                                                                                                        pdir = pdir.parent_path();
                                                                                                        depth++;
                                                                                                    }
                                                                                                }
                                                                                                VariantClear(&resultDocFN);
                                                                                            }
                                                                                        }
                                                                                    }
                                                                                    VariantClear(&resultActiveDoc);
                                                                                }
                                                                            }
                                                                        }
                                                                        if (!sln.empty())
                                                                        {
                                                                            for (auto& inst : found)
                                                                            {
                                                                                if (inst.pid == pid)
                                                                                {
                                                                                    inst.solutionPath = sln;
                                                                                }
                                                                            }
                                                                        }
                                                                        VariantClear(&resultSolution);
                                                                    }
                                                                }
                                                            }
                                                        }
                                                        VariantClear(&resultHwnd);
                                                    }
                                                }
                                                VariantClear(&resultMainWindow);
                                            }
                                            else
                                            {
                                                VariantClear(&resultMainWindow);
                                            }
                                        }
                                    }
                                    pDisp->Release();
                                }
                                pUnk->Release();
                            }
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
        }
        AppendLog(std::string("[vs] RefreshVSInstances: end, instances=") + std::to_string((int)g_vsList.size()));
    }

    void DrawVSUI()
    {
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
        {
            std::lock_guard<std::mutex> lock(g_vsMutexVS);
            local = g_vsList;
        }

        ImGui::Text("Found %d instance(s)", (int)local.size());
        ImGui::Separator();
        ImGui::BeginChild("vslist", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& inst : local)
        {
            ImGui::Text("PID: %lu", (unsigned long)inst.pid);
            if (!inst.windowTitle.empty())
                ImGui::TextDisabled("Title: %s", inst.windowTitle.c_str());
            if (!inst.exePath.empty())
                ImGui::TextDisabled("Path: %s", inst.exePath.c_str());
            if (!inst.solutionPath.empty())
                ImGui::TextDisabled("Solution: %s", inst.solutionPath.c_str());
            if (!inst.activeDocumentPath.empty())
                ImGui::TextDisabled("ActiveDocument: %s", inst.activeDocumentPath.c_str());
            ImGui::Separator();
        }
        ImGui::EndChild();

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
