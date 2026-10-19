cmake_minimum_required(VERSION 3.20)
project(pumpppt-playtime C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Folder holding the Dumper-7 chapter SDKs (the one containing the chapter folders)
set(POPPY_SDK_ROOT "${CMAKE_SOURCE_DIR}/../SDKS" CACHE PATH "Folder holding the Dumper-7 chapter SDKs")

# chapter -> SDK folder name / engine tag used for auto-detection
set(CH1 "4.27.2+UE4-ch1_pro")
set(TAG1 "4.27")
set(CH2 "4.27.2+UE4-ch2_pro")
set(TAG2 "4.27")
set(CH3 "5.1.1+UE5-ch3_pro")
set(TAG3 "5.1")
set(CH4 "5.3.2-0+UE5-ch4_pro")
set(TAG4 "5.3")
set(CH5 "5.6.0-0+UE5-ch5_pro")
set(TAG5 "5.6")

set(IMGUI_DIR   "${CMAKE_SOURCE_DIR}/third_party/imgui")
set(MINHOOK_DIR "${CMAKE_SOURCE_DIR}/third_party/minhook")
set(MODULE_DIR  "${CMAKE_BINARY_DIR}/modules")

file(GLOB MINHOOK_SRC "${MINHOOK_DIR}/src/*.c" "${MINHOOK_DIR}/src/hde/hde64.c")

add_library(imgui_static STATIC
    ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp ${IMGUI_DIR}/backends/imgui_impl_dx12.cpp)
target_include_directories(imgui_static PUBLIC ${IMGUI_DIR})

add_library(minhook_static STATIC ${MINHOOK_SRC})
target_include_directories(minhook_static PUBLIC ${MINHOOK_DIR}/include)

# ---- one module per chapter (internal, gets embedded into the single DLL) -------------------
set(MODULE_TARGETS "")
set(MODULE_FILES   "")
set(RC_LINES       "")

foreach(N 1 2 3 4 5)
    set(SDK_DIR "${POPPY_SDK_ROOT}/${CH${N}}/CppSDK")
    if(NOT EXISTS "${SDK_DIR}/SDK.hpp")
        message(WARNING "Chapter ${N}: ${SDK_DIR}/SDK.hpp not found, skipping")
        continue()
    endif()

    add_library(pumpppt_ch${N} SHARED
        src/dllmain.cpp src/render.cpp src/menu.cpp src/features.cpp src/resolve.cpp
        ${SDK_DIR}/SDK/Basic.cpp
        ${SDK_DIR}/SDK/CoreUObject_functions.cpp
        ${SDK_DIR}/SDK/Engine_functions.cpp)

    target_include_directories(pumpppt_ch${N} PRIVATE src ${SDK_DIR})
    target_compile_definitions(pumpppt_ch${N} PRIVATE
        MOD_CHAPTER=${N} "MOD_ENGINE_TAG=\"${TAG${N}}\"" WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(pumpppt_ch${N} PRIVATE imgui_static minhook_static d3d11 d3d12 dxgi d3dcompiler dwmapi version)
    if(MSVC)
        target_compile_options(pumpppt_ch${N} PRIVATE /bigobj /utf-8 /MP /permissive-)
    endif()
    set_target_properties(pumpppt_ch${N} PROPERTIES
        PREFIX "" OUTPUT_NAME "ch${N}"
        RUNTIME_OUTPUT_DIRECTORY "${MODULE_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${MODULE_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${MODULE_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${MODULE_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${MODULE_DIR}")

    math(EXPR RES_ID "100 + ${N}")
    list(APPEND MODULE_TARGETS pumpppt_ch${N})
    list(APPEND MODULE_FILES   "${MODULE_DIR}/ch${N}.dll")
    string(APPEND RC_LINES "${RES_ID} RCDATA \"${MODULE_DIR}/ch${N}.dll\"\n")
endforeach()

if(NOT MODULE_TARGETS)
    message(FATAL_ERROR "No chapter SDK found under ${POPPY_SDK_ROOT}. Pass -DPOPPY_SDK_ROOT=<path to SDKS>.")
endif()

# ---- the single DLL --------------------------------------------------------------------------
set(EMBED_RC "${CMAKE_BINARY_DIR}/embed_modules.rc")
file(WRITE "${EMBED_RC}" "${RC_LINES}")
set_source_files_properties("${EMBED_RC}" PROPERTIES OBJECT_DEPENDS "${MODULE_FILES}")

add_library(pumpppt-playtime SHARED src/loader/loader.cpp src/resolve.cpp "${EMBED_RC}")
add_dependencies(pumpppt-playtime ${MODULE_TARGETS})
target_include_directories(pumpppt-playtime PRIVATE src)
target_compile_definitions(pumpppt-playtime PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(pumpppt-playtime PRIVATE version)
if(MSVC)
    target_compile_options(pumpppt-playtime PRIVATE /utf-8 /permissive-)
endif()
set_target_properties(pumpppt-playtime PROPERTIES PREFIX "" OUTPUT_NAME "pumpppt-playtime")
