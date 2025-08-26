@echo off
echo Setting up Dear ImGui project...

REM Check if imgui directory already exists
if exist imgui (
    echo Dear ImGui already exists, skipping download...
) else (
    echo Downloading Dear ImGui...
    git clone https://github.com/ocornut/imgui.git
    if errorlevel 1 (
        echo Download failed! Please make sure Git is installed and you have internet connection.
        pause
        exit /b 1
    )
)

echo.
echo Project setup complete!
echo.
echo Next steps:
echo 1. Make sure you have CMake and a compiler installed (like Visual Studio or MinGW)
echo 2. Install GLFW library (you can use vcpkg: vcpkg install glfw3)
echo 3. Run the following commands to build the project:
echo    mkdir build
echo    cd build
echo    cmake ..
echo    cmake --build .
echo.
pause
