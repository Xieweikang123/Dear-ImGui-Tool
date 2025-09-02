#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <iostream>
#include <string>
#include <vector>

// 简化的WideToUtf8函数
std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

int main()
{
    std::cout << "=== Visual Studio DTE ROT Test ===" << std::endl;
    
    // 初始化COM
    std::cout << "1. Initializing COM..." << std::endl;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr))
    {
        std::cout << "   CoInitializeEx failed: " << std::hex << hr << std::endl;
        return 1;
    }
    std::cout << "   CoInitializeEx succeeded" << std::endl;
    
    // 设置COM安全
    std::cout << "2. Setting COM security..." << std::endl;
    hr = CoInitializeSecurity(NULL, -1, NULL, NULL,
        RPC_C_AUTHN_LEVEL_NONE, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
    if (FAILED(hr))
    {
        std::cout << "   CoInitializeSecurity failed: " << std::hex << hr << std::endl;
        // 尝试更宽松的设置
        hr = CoInitializeSecurity(NULL, -1, NULL, NULL,
            RPC_C_AUTHN_LEVEL_CONNECT, RPC_C_IMP_LEVEL_IDENTIFY, NULL, EOAC_NONE, NULL);
        if (FAILED(hr))
        {
            std::cout << "   CoInitializeSecurity retry failed: " << std::hex << hr << std::endl;
        }
        else
        {
            std::cout << "   CoInitializeSecurity retry succeeded" << std::endl;
        }
    }
    else
    {
        std::cout << "   CoInitializeSecurity succeeded" << std::endl;
    }
    
    // 获取Running Object Table
    std::cout << "3. Getting Running Object Table..." << std::endl;
    IRunningObjectTable* pRot = NULL;
    hr = GetRunningObjectTable(0, &pRot);
    if (FAILED(hr) || !pRot)
    {
        std::cout << "   GetRunningObjectTable failed: " << std::hex << hr << std::endl;
        CoUninitialize();
        return 1;
    }
    std::cout << "   GetRunningObjectTable succeeded" << std::endl;
    
    // 枚举ROT
    std::cout << "4. Enumerating ROT entries..." << std::endl;
    IEnumMoniker* pEnum = NULL;
    hr = pRot->EnumRunning(&pEnum);
    if (FAILED(hr) || !pEnum)
    {
        std::cout << "   EnumRunning failed: " << std::hex << hr << std::endl;
        pRot->Release();
        CoUninitialize();
        return 1;
    }
    std::cout << "   EnumRunning succeeded" << std::endl;
    
    // 扫描所有ROT条目
    std::vector<std::string> allEntries;
    std::vector<std::string> vsEntries;
    
    IMoniker* monikers[1];
    ULONG fetched = 0;
    int count = 0;
    
    std::cout << "5. Scanning ROT entries..." << std::endl;
    
    while (pEnum->Next(1, monikers, &fetched) == S_OK)
    {
        IBindCtx* pCtx = NULL;
        hr = CreateBindCtx(0, &pCtx);
        if (SUCCEEDED(hr))
        {
            LPOLESTR displayName = NULL;
            hr = monikers[0]->GetDisplayName(pCtx, NULL, &displayName);
            if (SUCCEEDED(hr))
            {
                std::wstring ws(displayName);
                std::string s = WideToUtf8(ws);
                allEntries.push_back(s);
                
                std::cout << "   Entry " << count << ": " << s << std::endl;
                
                // 检查是否是Visual Studio DTE对象
                if (s.find("!VisualStudio.DTE") != std::string::npos)
                {
                    vsEntries.push_back(s);
                    std::cout << "   *** Found Visual Studio DTE object! ***" << std::endl;
                    
                    // 尝试获取COM对象
                    IUnknown* pUnk = NULL;
                    hr = pRot->GetObject(monikers[0], &pUnk);
                    if (SUCCEEDED(hr) && pUnk)
                    {
                        std::cout << "   Successfully got COM object" << std::endl;
                        
                        // 获取IDispatch接口
                        IDispatch* pDisp = NULL;
                        hr = pUnk->QueryInterface(IID_IDispatch, (void**)&pDisp);
                        if (SUCCEEDED(hr) && pDisp)
                        {
                            std::cout << "   Successfully got IDispatch interface" << std::endl;
                            
                            // 尝试获取Solution属性
                            DISPID dispidSolution = 0;
                            OLECHAR* nameSolution = L"Solution";
                            hr = pDisp->GetIDsOfNames(IID_NULL, &nameSolution, 1, LOCALE_USER_DEFAULT, &dispidSolution);
                            if (SUCCEEDED(hr))
                            {
                                std::cout << "   Got Solution DISPID: " << dispidSolution << std::endl;
                                
                                // 调用Solution属性
                                VARIANT resultSolution;
                                VariantInit(&resultSolution);
                                DISPPARAMS noArgs = {0};
                                hr = pDisp->Invoke(dispidSolution, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &noArgs, &resultSolution, NULL, NULL);
                                if (SUCCEEDED(hr))
                                {
                                    std::cout << "   Successfully invoked Solution property, vt=" << resultSolution.vt << std::endl;
                                    
                                    if (resultSolution.vt == VT_DISPATCH && resultSolution.pdispVal)
                                    {
                                        std::cout << "   Solution is a dispatch object" << std::endl;
                                        
                                        // 尝试获取FullName属性
                                        IDispatch* pSolution = resultSolution.pdispVal;
                                        DISPID dispidFullName = 0;
                                        OLECHAR* nameFullName = L"FullName";
                                        hr = pSolution->GetIDsOfNames(IID_NULL, &nameFullName, 1, LOCALE_USER_DEFAULT, &dispidFullName);
                                        if (SUCCEEDED(hr))
                                        {
                                            std::cout << "   Got FullName DISPID: " << dispidFullName << std::endl;
                                            
                                            // 调用FullName属性
                                            VARIANT resultFullName;
                                            VariantInit(&resultFullName);
                                            hr = pSolution->Invoke(dispidFullName, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &noArgs, &resultFullName, NULL, NULL);
                                            if (SUCCEEDED(hr))
                                            {
                                                std::cout << "   Successfully invoked FullName property, vt=" << resultFullName.vt << std::endl;
                                                
                                                if (resultFullName.vt == VT_BSTR && resultFullName.bstrVal)
                                                {
                                                    std::wstring ws(resultFullName.bstrVal);
                                                    std::string slnPath = WideToUtf8(ws);
                                                    std::cout << "   *** Solution.FullName = " << slnPath << " ***" << std::endl;
                                                }
                                                else if (resultFullName.vt == VT_EMPTY || resultFullName.vt == VT_NULL)
                                                {
                                                    std::cout << "   Solution.FullName is empty - VS may be in Open Folder mode" << std::endl;
                                                }
                                                else
                                                {
                                                    std::cout << "   FullName unexpected vt=" << resultFullName.vt << std::endl;
                                                }
                                                VariantClear(&resultFullName);
                                            }
                                            else
                                            {
                                                std::cout << "   Failed to invoke FullName property: " << std::hex << hr << std::endl;
                                            }
                                        }
                                        else
                                        {
                                            std::cout << "   Failed to get FullName DISPID: " << std::hex << hr << std::endl;
                                        }
                                    }
                                    else if (resultSolution.vt == VT_EMPTY || resultSolution.vt == VT_NULL)
                                    {
                                        std::cout << "   Solution is empty - VS may be in Open Folder mode" << std::endl;
                                    }
                                    else
                                    {
                                        std::cout << "   Solution is not a dispatch object, vt=" << resultSolution.vt << std::endl;
                                    }
                                    VariantClear(&resultSolution);
                                }
                                else
                                {
                                    std::cout << "   Failed to invoke Solution property: " << std::hex << hr << std::endl;
                                }
                            }
                            else
                            {
                                std::cout << "   Failed to get Solution DISPID: " << std::hex << hr << std::endl;
                            }
                            pDisp->Release();
                        }
                        else
                        {
                            std::cout << "   Failed to get IDispatch interface: " << std::hex << hr << std::endl;
                        }
                        pUnk->Release();
                    }
                    else
                    {
                        std::cout << "   Failed to get COM object: " << std::hex << hr << std::endl;
                    }
                }
                
                CoTaskMemFree(displayName);
            }
            pCtx->Release();
        }
        
        monikers[0]->Release();
        count++;
    }
    
    std::cout << std::endl;
    std::cout << "=== Summary ===" << std::endl;
    std::cout << "Total ROT entries: " << count << std::endl;
    std::cout << "All entries:" << std::endl;
    for (const auto& entry : allEntries)
    {
        std::cout << "  " << entry << std::endl;
    }
    
    if (vsEntries.empty())
    {
        std::cout << std::endl;
        std::cout << "No Visual Studio DTE objects found!" << std::endl;
        std::cout << "Possible reasons:" << std::endl;
        std::cout << "1. No Visual Studio instances are running" << std::endl;
        std::cout << "2. Visual Studio is running but not registered in ROT" << std::endl;
        std::cout << "3. COM security/permission issues" << std::endl;
        std::cout << "4. Visual Studio is running as different user" << std::endl;
    }
    else
    {
        std::cout << std::endl;
        std::cout << "Visual Studio DTE objects found: " << vsEntries.size() << std::endl;
        for (const auto& vs : vsEntries)
        {
            std::cout << "  " << vs << std::endl;
        }
    }
    
    pEnum->Release();
    pRot->Release();
    CoUninitialize();
    
    std::cout << std::endl;
    std::cout << "Test completed. Press any key to exit..." << std::endl;
    std::cin.get();
    return 0;
}
