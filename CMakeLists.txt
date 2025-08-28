

cmake_minimum_required(VERSION 3.16)
project(DearImGuiExample)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(USE_D3D11 "Use Win32 + DirectX11 backend on Windows" ON)
option(USE_OPENGL2 "Use ImGui OpenGL2 backend instead of OpenGL3" OFF)

# User-overridable cache variables for GLFW (used when not using D3D11)
set(GLFW_INCLUDE_DIR "" CACHE PATH "Path to GLFW include directory (contains GLFW/glfw3.h)")
set(GLFW_LIBRARY "" CACHE FILEPATH "Path to GLFW library file (e.g., glfw3.lib)")

# Add Dear ImGui source files
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
)

if(WIN32 AND USE_D3D11)
    list(APPEND IMGUI_SOURCES ${IMGUI_DIR}/backends/imgui_impl_win32.cpp)
    list(APPEND IMGUI_SOURCES ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp)
    add_compile_definitions(IMGUI_USE_D3D11)
else()
    list(APPEND IMGUI_SOURCES ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp)
    if(USE_OPENGL2)
        list(APPEND IMGUI_SOURCES ${IMGUI_DIR}/backends/imgui_impl_opengl2.cpp)
        add_compile_definitions(IMGUI_USE_OPENGL2)
    else()
        list(APPEND IMGUI_SOURCES ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
    endif()
endif()

# App sources
set(APP_SOURCES
    main.cpp
    src/replace_tool.cpp
    src/vs_inspector.cpp
    src/feature_manager.cpp
    src/word_reminder.cpp
)

# Create executable (use WinMain entry on Windows+D3D11)
if(WIN32 AND USE_D3D11)
    add_executable(${PROJECT_NAME} WIN32
        ${APP_SOURCES}
        ${IMGUI_SOURCES}
    )
else()
    add_executable(${PROJECT_NAME}
        ${APP_SOURCES}
        ${IMGUI_SOURCES}
    )
endif()

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

if(WIN32 AND USE_D3D11)
    # Link DirectX 11 libraries
    target_link_libraries(${PROJECT_NAME} d3d11 dxgi d3dcompiler ole32 oleaut32)
else()
    # Non-D3D11 path: find and link GLFW + OpenGL
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GLFW QUIET glfw3)
        if(GLFW_FOUND)
            target_include_directories(${PROJECT_NAME} PRIVATE ${GLFW_INCLUDE_DIRS})
            target_link_libraries(${PROJECT_NAME} ${GLFW_LIBRARIES})
            target_compile_options(${PROJECT_NAME} PRIVATE ${GLFW_CFLAGS_OTHER})
            message(STATUS "Found GLFW via pkg-config")
        endif()
    endif()

    if(GLFW_INCLUDE_DIR)
        target_include_directories(${PROJECT_NAME} PRIVATE ${GLFW_INCLUDE_DIR})
        set(_glfw_headers_found TRUE)
    endif()
    if(GLFW_LIBRARY)
        target_link_libraries(${PROJECT_NAME} ${GLFW_LIBRARY})
        set(_glfw_lib_found TRUE)
    endif()

    if(NOT _glfw_headers_found OR NOT _glfw_lib_found)
        if(NOT _glfw_headers_found)
            find_path(GLFW_INCLUDE_DIR NAMES GLFW/glfw3.h
                PATHS
                /usr/include
                /usr/local/include
                /opt/local/include
                /sw/include
                "C:/Program Files/GLFW/include"
                "C:/Program Files (x86)/GLFW/include"
                "D:/soft/glfw-3.4.bin.WIN64/include"
            )
            if(GLFW_INCLUDE_DIR)
                target_include_directories(${PROJECT_NAME} PRIVATE ${GLFW_INCLUDE_DIR})
                message(STATUS "Found GLFW headers: ${GLFW_INCLUDE_DIR}")
                set(_glfw_headers_found TRUE)
            endif()
        endif()
        if(NOT _glfw_lib_found)
            find_library(GLFW_LIBRARY NAMES glfw3 glfw
                PATHS
                /usr/lib
                /usr/local/lib
                /opt/local/lib
                /sw/lib
                "C:/Program Files/GLFW/lib"
                "C:/Program Files (x86)/GLFW/lib"
                "D:/soft/glfw-3.4.bin.WIN64/lib-vc2022"
                "D:/soft/glfw-3.4.bin.WIN64/lib-vc2019"
                "D:/soft/glfw-3.4.bin.WIN64/lib-mingw-w64"
            )
            if(GLFW_LIBRARY)
                target_link_libraries(${PROJECT_NAME} ${GLFW_LIBRARY})
                message(STATUS "Found GLFW library: ${GLFW_LIBRARY}")
                set(_glfw_lib_found TRUE)
            endif()
        endif()
    endif()

    if(NOT _glfw_headers_found OR NOT _glfw_lib_found)
        message(FATAL_ERROR "GLFW not found! Set GLFW_INCLUDE_DIR and GLFW_LIBRARY, or install GLFW.")
    endif()

    if(WIN32)
        target_link_libraries(${PROJECT_NAME} opengl32 gdi32 ole32 oleaut32)
    elseif(APPLE)
        find_library(OPENGL_LIBRARY OpenGL)
        target_link_libraries(${PROJECT_NAME} ${OPENGL_LIBRARY})
    else()
        find_package(OpenGL REQUIRED)
        target_link_libraries(${PROJECT_NAME} OpenGL::GL)
    endif()
endif()

# Compiler options
if(MSVC)
    # Ensure MSVC treats sources as UTF-8 to avoid codepage mis-parsing
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /utf-8)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
endif()
