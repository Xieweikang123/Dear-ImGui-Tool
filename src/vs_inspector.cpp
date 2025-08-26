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

    // Helpers to flatten COM interaction and reduce nesting
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
                if (SUCCEEDED(pMainWin->Invoke(dispidHWnd, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &noArgs, &resultHwnd, NULL, NULL)))
                {
                    LONG hwndVal = 0;
                    if (resultHwnd.vt == VT_I4) hwndVal = resultHwnd.lVal; else if (resultHwnd.vt == VT_I2) hwndVal = resultHwnd.iVal;
                    if (hwndVal)
                    {
                        GetWindowThreadProcessId((HWND)(INT_PTR)hwndVal, &pidOut);
                        ok = (pidOut != 0);
                        if (ok) AppendLog(std::string("[vs] DTE hwnd=") + std::to_string((long)hwndVal) + std::string(" pid=") + std::to_string((unsigned long)pidOut));
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

    static void ProcessDteMoniker(IRunningObjectTable* pRot, IMoniker* pMoniker, std::vector<VSInstance>& found)
    {
        IUnknown* pUnk = NULL; HRESULT hrGetObj = pRot->GetObject(pMoniker, &pUnk);
        if (FAILED(hrGetObj) || !pUnk) { AppendLog(std::string("[vs] GetObject(moniker) failed hr=") + std::to_string((long)hrGetObj)); return; }
        IDispatch* pDisp = NULL; HRESULT hrQI = pUnk->QueryInterface(IID_IDispatch, (void**)&pDisp);
        if (FAILED(hrQI) || !pDisp) { AppendLog(std::string("[vs] QueryInterface(IDispatch) failed hr=") + std::to_string((long)hrQI)); pUnk->Release(); return; }

        DWORD pid = 0; if (!GetPidFromDTE(pDisp, pid) || pid == 0) { pDisp->Release(); pUnk->Release(); return; }
        std::string sln; (void)TryGetSolutionFullName(pDisp, sln);
        if (sln.empty()) { TryFillFromActiveDocument(pDisp, pid, found, sln); }
        if (!sln.empty())
        {
            for (auto& inst : found) { if (inst.pid == pid) { inst.solutionPath = sln; AppendLog(std::string("[vs] pid ") + std::to_string((unsigned long)pid) + std::string(" Set solutionPath")); } }
        }
        pDisp->Release(); pUnk->Release();
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
                    AppendLog(std::string("[vs] found devenv.exe pid=") + std::to_string((unsigned long)inst.pid) + (inst.exePath.empty() ? std::string(" path=<unknown>") : std::string(" path=") + inst.exePath));
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
            AppendLog(std::string("[vs] pid ") + std::to_string((unsigned long)inst.pid) + std::string(" title=") + (inst.windowTitle.empty()?"<none>":inst.windowTitle));
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
                            // Use flattened helper to avoid deep nesting
                            ProcessDteMoniker(pRot, pMoniker, found);
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
        }
        AppendLog(std::string("[vs] RefreshVSInstances: end, instances=") + std::to_string((int)g_vsList.size()));
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
