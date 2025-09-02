@echo off
echo Building Dear ImGui project (compile only)...

REM Kill any existing DearImGuiExample.exe processes
echo Stopping any running DearImGuiExample.exe processes...
taskkill /f /im DearImGuiExample.exe >nul 2>&1
if errorlevel 1 (
    echo No running DearImGuiExample.exe processes found.
) else (
    echo Successfully stopped DearImGuiExample.exe processes.
)

REM Check if imgui directory exists
if not exist imgui (
    echo Error: imgui directory not found!
    echo Please run setup.bat first to download Dear ImGui
    pause
    exit /b 1
)

REM Create build directory
if not exist build mkdir build
cd build

REM Run CMake configuration
echo Configuring project...
cmake ..
if errorlevel 1 (
    echo CMake configuration failed!
    pause
    exit /b 1
)

REM Build project
echo Building project...
cmake --build . --config Release
if errorlevel 1 (
    echo Build failed!
    pause
    exit /b 1
)

echo.
echo Build successful!
echo Executable location: build\Release\DearImGuiExample.exe
echo.
echo Ready to run - use build.bat to compile and run, or run the executable manually
