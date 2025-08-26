@echo off
echo Installing GLFW for Dear ImGui project...

echo.
echo This script will help you install GLFW. You have several options:
echo.
echo 1. Use vcpkg (recommended for Windows)
echo 2. Download pre-built binaries
echo 3. Build from source
echo.

set /p choice="Choose option (1-3): "

if "%choice%"=="1" goto vcpkg
if "%choice%"=="2" goto download
if "%choice%"=="3" goto build
goto invalid

:vcpkg
echo.
echo Installing GLFW via vcpkg...
echo.
echo First, make sure you have vcpkg installed. If not, run:
echo git clone https://github.com/Microsoft/vcpkg.git
echo cd vcpkg
echo .\bootstrap-vcpkg.bat
echo .\vcpkg integrate install
echo.
echo Then run:
echo .\vcpkg install glfw3
echo.
pause
goto end

:download
echo.
echo Downloading GLFW pre-built binaries...
echo.
echo Please download GLFW from: https://www.glfw.org/download.html
echo.
echo For Windows:
echo 1. Download the 64-bit Windows pre-compiled binaries
echo 2. Extract to C:\glfw
echo 3. Add C:\glfw\lib-vc2022 to your PATH
echo.
pause
goto end

:build
echo.
echo Building GLFW from source...
echo.
echo Please follow these steps:
echo 1. Download GLFW source from: https://www.glfw.org/download.html
echo 2. Extract and build using CMake
echo 3. Install to a known location
echo.
pause
goto end

:invalid
echo Invalid choice. Please run the script again.
pause
goto end

:end
echo.
echo After installing GLFW, you can run build.bat to build the project.
pause
