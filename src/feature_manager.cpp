#include "feature_manager.h"
#include "replace_tool.h"
#include "vs_inspector.h"
#include "word_reminder.h"
#include "imgui.h"

FeatureManager& FeatureManager::GetInstance()
{
    static FeatureManager instance;
    return instance;
}

void FeatureManager::Initialize()
{
    RegisterFeatures();
    
    // Initialize all features
    for (auto& feature : features)
    {
        if (feature.initFunction)
        {
            feature.initFunction();
        }
    }
}

void FeatureManager::Cleanup()
{
    // Cleanup all features
    for (auto& feature : features)
    {
        if (feature.cleanupFunction)
        {
            feature.cleanupFunction();
        }
    }
}

void FeatureManager::RegisterFeatures()
{
    // Register Replace Tool
    features.push_back({
        "String Replace Tool",
        "Replace strings in files and filenames",
        true,
        []() { ReplaceTool::DrawReplaceUI(); },
        nullptr, // No init needed
        nullptr  // No cleanup needed
    });
    
    // Register VS Inspector
    features.push_back({
        "Visual Studio Inspector", 
        "Inspect running Visual Studio instances",
        true,
        []() { VSInspector::DrawVSUI(); },
        nullptr, // No init needed
        nullptr  // No cleanup needed
    });
    
    // Register Word Reminder
    features.push_back({
        "单词学习提醒",
        "英语单词学习定时提醒工具",
        true,
        []() { WordReminder::DrawUI(); },
        []() { WordReminder::Initialize(); },
        []() { WordReminder::Cleanup(); }
    });
}

void FeatureManager::DrawAllFeatures()
{
    for (auto& feature : features)
    {
        if (feature.enabled && feature.drawFunction)
        {
            feature.drawFunction();
        }
    }
}

void FeatureManager::EnableFeature(const std::string& name, bool enable)
{
    for (auto& feature : features)
    {
        if (feature.name == name)
        {
            feature.enabled = enable;
            break;
        }
    }
}

bool FeatureManager::IsFeatureEnabled(const std::string& name)
{
    for (const auto& feature : features)
    {
        if (feature.name == name)
        {
            return feature.enabled;
        }
    }
    return false;
}

void FeatureManager::DrawFeatureSelector()
{
    if (ImGui::Begin("Feature Manager", &showFeatureSelector))
    {
        ImGui::Text("Enable/Disable Features");
        ImGui::Separator();
        
        for (auto& feature : features)
        {
            bool enabled = feature.enabled;
            if (ImGui::Checkbox(feature.name.c_str(), &enabled))
            {
                feature.enabled = enabled;
            }
            
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", feature.description.c_str());
            }
        }
        
        ImGui::Separator();
        
        if (ImGui::Button("Enable All"))
        {
            for (auto& feature : features)
            {
                feature.enabled = true;
            }
        }
        
        ImGui::SameLine();
        
        if (ImGui::Button("Disable All"))
        {
            for (auto& feature : features)
            {
                feature.enabled = false;
            }
        }
    }
    ImGui::End();
}
