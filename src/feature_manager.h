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

private:
    FeatureManager() = default;
    ~FeatureManager() = default;
    FeatureManager(const FeatureManager&) = delete;
    FeatureManager& operator=(const FeatureManager&) = delete;
    
    void RegisterFeatures();
    
    std::vector<FeatureInfo> features;
    bool showFeatureSelector = false;
};
