#pragma once

namespace VSInspector
{
    // Refresh the list of running Visual Studio instances (Windows only; no-op elsewhere)
    void Refresh();

    // Draw the Visual Studio inspector UI (shows instances and shared log)
    void DrawVSUI();
}
