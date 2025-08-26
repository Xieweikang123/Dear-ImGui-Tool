#pragma once

#include <string>
#include <vector>

namespace ReplaceTool
{
    // Append a line to the shared application log (also mirrored to file when enabled by the tool)
    void AppendLog(const std::string& line);

    // Draw the Replace Tool UI window
    void DrawReplaceUI();

    // Draw a read-only view of the shared log
    void DrawSharedLog(const char* id, float height);
}
