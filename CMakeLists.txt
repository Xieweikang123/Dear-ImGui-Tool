

cmake_minimum_required(VERSION 3.16)
project(DearImGuiExample)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add Dear ImGui source files
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

# Create executable
add_executable(${PROJECT_NAME} 
    main.cpp
    ${IMGUI_SOURCES}
)

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

# Try to find GLFW using multiple methods
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

# If GLFW not found via pkg-config, try find_library
if(NOT GLFW_FOUND)
    find_library(GLFW_LIBRARY NAMES glfw3 glfw)
    if(GLFW_LIBRARY)
        target_link_libraries(${PROJECT_NAME} ${GLFW_LIBRARY})
        message(STATUS "Found GLFW library: ${GLFW_LIBRARY}")
    else()
        # Try to find GLFW headers manually
        find_path(GLFW_INCLUDE_DIR NAMES GLFW/glfw3.h
            PATHS
            /usr/include
            /usr/local/include
            /opt/local/include
            /sw/include
            "C:/Program Files/GLFW/include"
            "C:/Program Files (x86)/GLFW/include"
        )
        
        if(GLFW_INCLUDE_DIR)
            target_include_directories(${PROJECT_NAME} PRIVATE ${GLFW_INCLUDE_DIR})
            message(STATUS "Found GLFW headers: ${GLFW_INCLUDE_DIR}")
            
            # Try to find GLFW library in common locations
            find_library(GLFW_LIBRARY NAMES glfw3 glfw
                PATHS
                /usr/lib
                /usr/local/lib
                /opt/local/lib
                /sw/lib
                "C:/Program Files/GLFW/lib"
                "C:/Program Files (x86)/GLFW/lib"
            )
            
            if(GLFW_LIBRARY)
                target_link_libraries(${PROJECT_NAME} ${GLFW_LIBRARY})
                message(STATUS "Found GLFW library: ${GLFW_LIBRARY}")
            else()
                message(FATAL_ERROR "GLFW library not found! Please install GLFW.")
            endif()
        else()
            message(FATAL_ERROR "GLFW not found! Please install GLFW library and headers.")
        endif()
    endif()
endif()

# Link OpenGL
if(WIN32)
    target_link_libraries(${PROJECT_NAME} opengl32)
elseif(APPLE)
    find_library(OPENGL_LIBRARY OpenGL)
    target_link_libraries(${PROJECT_NAME} ${OPENGL_LIBRARY})
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(${PROJECT_NAME} OpenGL::GL)
endif()

# Additional libraries for Windows
if(WIN32)
    target_link_libraries(${PROJECT_NAME} gdi32)
endif()

# Compiler options
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
endif()
