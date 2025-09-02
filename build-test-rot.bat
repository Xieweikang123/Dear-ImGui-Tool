@echo off
echo Building test_rot.cpp...

REM Create build directory for test_rot
if not exist build_test_rot mkdir build_test_rot
cd build_test_rot

REM Copy the test_rot CMakeLists to the build directory
echo Copying CMakeLists.txt...
copy ..\CMakeLists_test_rot.txt CMakeLists.txt

REM Run CMake configuration
echo Configuring test_rot project...
cmake .
if errorlevel 1 (
    echo CMake configuration failed!
    pause
    exit /b 1
)

REM Build project
echo Building test_rot...
cmake --build . --config Release
if errorlevel 1 (
    echo Build failed!
    pause
    exit /b 1
)

echo.
echo Build successful!
echo Executable location: build_test_rot\Release\test_rot.exe
echo.
echo You can now run: build_test_rot\Release\test_rot.exe

cd ..
