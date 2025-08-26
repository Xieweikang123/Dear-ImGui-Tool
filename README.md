# Dear ImGui Minimal Example

A minimal example project using the Dear ImGui library to create a simple graphical user interface.

## Features

- Basic ImGui window
- Text display
- Slider control
- Button interaction
- Real-time FPS display
- Time display

## Quick Start (Windows)

### Method 1: Using provided scripts (Recommended)

1. **Download Dear ImGui**
   ```cmd
   setup.bat
   ```

2. **Install GLFW** (if not already installed)
   ```cmd
   install_glfw.bat
   ```

3. **Build and run**
   ```cmd
   build.bat
   ```

### Method 2: Manual build

1. **Install dependencies**
   - Install [CMake](https://cmake.org/download/)
   - Install [Visual Studio](https://visualstudio.microsoft.com/) or [MinGW](https://www.mingw-w64.org/)
   - Install GLFW (recommended using vcpkg):
     ```cmd
     vcpkg install glfw3
     ```

2. **Download Dear ImGui**
   ```cmd
   git clone https://github.com/ocornut/imgui.git
   ```

3. **Build project**
   ```cmd
   mkdir build
   cd build
   cmake ..
   cmake --build . --config Release
   ```

4. **Run**
   ```cmd
   Release\DearImGuiExample.exe
   ```

## Linux/macOS Build

### Install dependencies

#### Ubuntu/Debian
```bash
sudo apt-get install build-essential cmake
sudo apt-get install libglfw3-dev
sudo apt-get install libgl1-mesa-dev
```

#### macOS
```bash
brew install cmake
brew install glfw
```

### Build steps
```bash
git clone https://github.com/ocornut/imgui.git
mkdir build
cd build
cmake ..
make
./DearImGuiExample
```

## Project Structure

```
Dear-ImGui-Tool/
├── CMakeLists.txt      # CMake build configuration
├── main.cpp           # Main program file
├── README.md          # Project documentation
├── setup.bat          # Windows auto-setup script
├── build.bat          # Windows auto-build script
├── install_glfw.bat   # GLFW installation helper
└── imgui/             # Dear ImGui library files (needs download)
```

## Code Explanation

This example includes the following main components:

1. **GLFW Initialization**: Create window and OpenGL context
2. **ImGui Setup**: Initialize ImGui and configure style
3. **Main Loop**: Handle events, update UI, render
4. **UI Components**: Text, slider, button and other basic controls

### Main Features

- `ImGui::Begin()` / `ImGui::End()` - Create windows
- `ImGui::Text()` - Display text
- `ImGui::Button()` - Create buttons
- `ImGui::SliderFloat()` - Create float sliders
- `ImGui::GetTime()` - Get application runtime
- `io.Framerate` - Get current FPS

## Customizing UI

You can add more controls by modifying the UI code in `main.cpp`:

```cpp
// Text input
static char text[256] = "";
ImGui::InputText("Input text", text, sizeof(text));

// Checkbox
static bool checkbox = false;
ImGui::Checkbox("Checkbox", &checkbox);

// Color picker
static float color[3] = {1.0f, 0.0f, 0.0f};
ImGui::ColorEdit3("Color", color);

// Dropdown menu
static int item = 0;
const char* items[] = {"Item 1", "Item 2", "Item 3"};
ImGui::Combo("Dropdown", &item, items, IM_ARRAYSIZE(items));
```

## Troubleshooting

### Windows Common Issues

1. **CMake can't find GLFW**
   - Make sure GLFW is installed: `vcpkg install glfw3`
   - Or manually download GLFW and set environment variables

2. **Compilation errors**
   - Ensure you're using C++17 or higher
   - Check if Visual Studio has C++ development tools installed

3. **Runtime errors**
   - Make sure OpenGL drivers are up to date
   - Check if Visual C++ Redistributable is missing

### Linux/macOS Common Issues

1. **OpenGL not found**
   - Ubuntu: `sudo apt-get install libgl1-mesa-dev`
   - macOS: Usually pre-installed

2. **Permission errors**
   - Make sure executable has permissions: `chmod +x DearImGuiExample`

## GLFW Installation

If you're having trouble with GLFW, you can:

1. **Use vcpkg** (recommended for Windows):
   ```cmd
   git clone https://github.com/Microsoft/vcpkg.git
   cd vcpkg
   .\bootstrap-vcpkg.bat
   .\vcpkg integrate install
   .\vcpkg install glfw3
   ```

2. **Download pre-built binaries**:
   - Visit https://www.glfw.org/download.html
   - Download Windows 64-bit pre-compiled binaries
   - Extract to a known location and add to PATH

3. **Build from source**:
   - Download source from GLFW website
   - Build using CMake
   - Install to system

## Extending Functionality

This minimal example can be easily extended:

- Add more windows and controls
- Implement custom themes
- Add image and texture support
- Integrate other libraries (audio, networking, etc.)

## License

This example follows the MIT license of Dear ImGui.
