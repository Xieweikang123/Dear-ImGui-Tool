#pragma once

#include <vector>
#include <string>
#include <functional>

// Forward declarations of feature modules
namespace ReplaceTool { void DrawReplaceUI(); }
namespace VSInspector { void DrawVSUI(); }
namespace WordReminder { void DrawUI(); }

struct FeatureInfo
{
    std::string name;
    std::string description;
    bool enabled;
    std::function<void()> drawFunction;
    std::function<void()> initFunction;
    std::function<void()> cleanupFunction;
};

class FeatureManager
{
public:
    static FeatureManager& GetInstance();
    
    void Initialize();
    void Cleanup();
    void DrawAllFeatures();
    
    // Feature management
    void EnableFeature(const std::string& name, bool enable);
    bool IsFeatureEnabled(const std::string& name);
    const std::vector<FeatureInfo>& GetFeatures() const { return features; }
    
    // Draw feature selection UI
    void DrawFeatureSelector();
    
    // Window visibility control
    void ShowFeatureSelector() { 
        if (showFeatureSelector) {
            // 如果窗口已经显示，标记为需要重新置顶
            needBringToFront = true;
        }
        showFeatureSelector = true; 
    }
    void HideFeatureSelector() { showFeatureSelector = false; }
    bool IsFeatureSelectorVisible() const { return showFeatureSelector; }

private:
    FeatureManager() = default;
    ~FeatureManager() = default;
    FeatureManager(const FeatureManager&) = delete;
    FeatureManager& operator=(const FeatureManager&) = delete;
    
    void RegisterFeatures();
    void LoadState();
    void SaveState() const;
    
    std::vector<FeatureInfo> features;
    bool showFeatureSelector = false;
    bool needBringToFront = false;
};
