# 功能开发指南

## 概述

这个项目采用了模块化的设计架构，可以轻松添加新功能而不会影响现有功能。

## 架构设计

### 核心组件

1. **FeatureManager** - 功能管理器
   - 统一管理所有功能模块
   - 提供功能的启用/禁用控制
   - 处理功能的初始化和清理

2. **功能模块** - 独立的功能实现
   - 每个功能都有自己的命名空间
   - 包含独立的UI和业务逻辑
   - 可以独立开发和测试

### 现有功能

- **ReplaceTool** - 字符串替换工具
- **VSInspector** - Visual Studio实例检查器
- **NewFeature** - 新功能模板

## 添加新功能的步骤

### 1. 创建功能模块

#### 创建头文件 (`src/your_feature.h`)

```cpp
#pragma once

#include <string>
#include <vector>

namespace YourFeature
{
    // 初始化功能模块
    void Initialize();
    
    // 清理功能模块
    void Cleanup();
    
    // 绘制UI
    void DrawUI();
    
    // 获取功能名称
    const char* GetFeatureName();
    
    // 检查功能是否启用
    bool IsEnabled();
    
    // 启用/禁用功能
    void SetEnabled(bool enabled);
}
```

#### 创建实现文件 (`src/your_feature.cpp`)

```cpp
#include "your_feature.h"
#include "imgui.h"
#include <string>
#include <vector>
#include <memory>

namespace YourFeature
{
    // 内部状态管理
    struct FeatureState
    {
        bool enabled = true;
        bool windowOpen = true;
        // 添加你的功能特定状态变量
        
        FeatureState()
        {
            // 初始化默认值
        }
    };
    
    static std::unique_ptr<FeatureState> g_state;
    
    void Initialize()
    {
        if (!g_state)
        {
            g_state = std::make_unique<FeatureState>();
        }
        
        // 加载配置
        // 初始化资源
    }
    
    void Cleanup()
    {
        if (g_state)
        {
            // 保存配置
            // 清理资源
            g_state.reset();
        }
    }
    
    const char* GetFeatureName()
    {
        return "Your Feature Name";
    }
    
    bool IsEnabled()
    {
        return g_state && g_state->enabled;
    }
    
    void SetEnabled(bool enabled)
    {
        if (g_state)
        {
            g_state->enabled = enabled;
        }
    }
    
    void DrawUI()
    {
        if (!g_state || !g_state->enabled)
            return;
            
        // 创建主窗口
        if (!ImGui::Begin("Your Feature", &g_state->windowOpen))
        {
            ImGui::End();
            return;
        }
        
        // 实现你的UI逻辑
        ImGui::Text("Hello from your feature!");
        
        ImGui::End();
    }
}
```

### 2. 注册新功能

在 `src/feature_manager.cpp` 的 `RegisterFeatures()` 函数中添加：

```cpp
void FeatureManager::RegisterFeatures()
{
    // ... 现有功能 ...
    
    // 注册你的新功能
    features.push_back({
        "Your Feature Name",
        "Description of your feature",
        true,
        []() { YourFeature::DrawUI(); },
        []() { YourFeature::Initialize(); },
        []() { YourFeature::Cleanup(); }
    });
}
```

### 3. 更新构建配置

在 `CMakeLists.txt` 中添加新源文件：

```cmake
set(APP_SOURCES
    main.cpp
    src/replace_tool.cpp
    src/vs_inspector.cpp
    src/new_feature.cpp
    src/feature_manager.cpp
    src/your_feature.cpp  # 添加这一行
)
```

### 4. 添加头文件包含

在 `src/feature_manager.cpp` 中添加：

```cpp
#include "your_feature.h"
```

## 最佳实践

### 1. 状态管理

- 使用 `FeatureState` 结构体管理功能状态
- 使用 `std::unique_ptr` 管理内存
- 在 `Initialize()` 中加载配置
- 在 `Cleanup()` 中保存配置和清理资源

### 2. UI设计

- 使用 ImGui 组件创建用户界面
- 提供合理的默认值和用户反馈
- 考虑窗口的打开/关闭状态
- 添加工具提示和帮助信息

### 3. 错误处理

- 检查状态有效性
- 提供错误信息和用户提示
- 优雅处理异常情况

### 4. 性能考虑

- 避免在UI渲染循环中进行耗时操作
- 使用异步处理处理长时间运行的任务
- 合理管理内存和资源

## 示例

参考 `src/new_feature.cpp` 作为新功能开发的模板。

## 调试和测试

1. 编译并运行程序
2. 通过 "Tools" -> "Feature Manager" 启用/禁用功能
3. 检查功能是否正常工作
4. 验证初始化和清理是否正常

## 注意事项

- 保持功能模块的独立性
- 遵循现有的命名约定
- 测试功能的启用/禁用功能
- 确保清理代码正确执行
