#include "feature_manager.h"
#include "replace_tool.h"
#include "vs_inspector.h"
#include "word_reminder.h"
#include "imgui.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
    static std::filesystem::path GetConfigPath()
    {
        // Store next to the executable or current working directory
        std::error_code ec;
        std::filesystem::path exeDir;
#ifdef _WIN32
        wchar_t exePathW[MAX_PATH] = {0};
        if (GetModuleFileNameW(NULL, exePathW, MAX_PATH) > 0)
            exeDir = std::filesystem::path(exePathW).parent_path();
#endif
        if (exeDir.empty()) exeDir = std::filesystem::current_path(ec);
        if (ec) return std::filesystem::path("feature_state.ini");
        return exeDir / "feature_state.ini";
    }
}

FeatureManager& FeatureManager::GetInstance()
{
    static FeatureManager instance;
    return instance;
}

void FeatureManager::Initialize()
{
    RegisterFeatures();
    LoadState();
    
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
    SaveState();
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
            SaveState();
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
                SaveState();
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
            SaveState();
        }
        
        ImGui::SameLine();
        
        if (ImGui::Button("Disable All"))
        {
            for (auto& feature : features)
            {
                feature.enabled = false;
            }
            SaveState();
        }
    }
    ImGui::End();
}

void FeatureManager::LoadState()
{
    const auto path = GetConfigPath();
    std::error_code ec; if (!std::filesystem::exists(path, ec)) return;
    std::ifstream ifs(path.string(), std::ios::in | std::ios::binary);
    if (!ifs) return;
    std::string line;
    // Simple ini: name=0/1 per line
    while (std::getline(ifs, line))
    {
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        std::string key = line.substr(0, pos);
        std::string val = line.substr(pos + 1);
        bool on = (val == "1" || val == "true" || val == "True");
        for (auto& f : features)
        {
            if (f.name == key) { f.enabled = on; break; }
        }
    }
}

void FeatureManager::SaveState() const
{
    const auto path = GetConfigPath();
    std::ofstream ofs(path.string(), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!ofs) return;
    for (const auto& f : features)
    {
        ofs << f.name << '=' << (f.enabled ? '1' : '0') << '\n';
    }
}
