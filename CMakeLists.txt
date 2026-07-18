cmake_minimum_required(VERSION 3.24)
project(pcbview VERSION 1.09 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Vulkan comes from the SDK (VULKAN_SDK env var). Everything else arrives via
# FetchContent -- no package manager to install.
find_package(Vulkan REQUIRED)

include(FetchContent)

# Clipper2: robust integer-coordinate 2D booleans. Pinned to 1.5.x deliberately;
# 2.0 is a major-version API change we have not evaluated.
set(CLIPPER2_UTILS OFF CACHE BOOL "" FORCE)
set(CLIPPER2_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLIPPER2_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(clipper2
    GIT_REPOSITORY https://github.com/AngusJohnson/Clipper2.git
    GIT_TAG Clipper2_1.5.4
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR CPP
)
FetchContent_MakeAvailable(clipper2)

# earcut.hpp: header-only ear-clipping triangulation. SOURCE_SUBDIR points at a
# directory that does not exist on purpose -- it suppresses add_subdirectory so
# we skip its tests/benchmarks and just take the header.
FetchContent_Declare(earcut
    GIT_REPOSITORY https://github.com/mapbox/earcut.hpp.git
    GIT_TAG v2.2.4
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR header-only-no-cmake
)
FetchContent_MakeAvailable(earcut)

# glm: header-only maths. Same no-cmake trick as earcut.
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR header-only-no-cmake
)
FetchContent_MakeAvailable(glm)

# miniz: single-file zip reader (MIT), for opening zipped gerber packages. Build
# its amalgamated source directly rather than via its CMake, which installs more
# than we need.
FetchContent_Declare(miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG 3.0.2
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR no-cmake-here
)
FetchContent_MakeAvailable(miniz)
# The repo ships miniz split across several .c files rather than amalgamated;
# compile them straight into the target. Third-party, so warnings are silenced.
set(MINIZ_SOURCES
    ${miniz_SOURCE_DIR}/miniz.c
    ${miniz_SOURCE_DIR}/miniz_tdef.c
    ${miniz_SOURCE_DIR}/miniz_tinfl.c
    ${miniz_SOURCE_DIR}/miniz_zip.c
)

# cgltf: single-header glTF/GLB reader (MIT), for loading the component bodies
# that kicad-cli exports. Header-only; same no-cmake trick as earcut/glm.
FetchContent_Declare(cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG v1.15
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR no-cmake-here
)
FetchContent_MakeAvailable(cgltf)

# Intel Open Image Denoise: the neural denoiser for the path tracer (Apache-2.0,
# GPL-3-compatible). Prebuilt binary package -- header + import lib + runtime DLLs.
FetchContent_Declare(oidn
    URL https://github.com/RenderKit/oidn/releases/download/v2.5.0/oidn-2.5.0.x64.windows.zip
    URL_HASH SHA256=6ae0474ef7606d68647c1e2c2842832d6af01128ee84a523d30368a91013b707
)
FetchContent_MakeAvailable(oidn)
set(OIDN_DIR "${oidn_SOURCE_DIR}")

# ---------------------------------------------------------------------------
# Qt -- the GUI. Used under LGPL-3.0 via option 4(d)(1): DYNAMIC LINKING ONLY.
#
# Do NOT statically link Qt here. That is a licence violation, not a build
# optimisation -- see NOTICE.md. Only LGPL-licensed modules may be added; some
# Qt modules are GPL-3.0-or-commercial only and would relicense all of pcbview.
# Core, Gui and Widgets are LGPL-3.0.
#
# Qt cannot be FetchContent'd; it must be installed. Point CMAKE_PREFIX_PATH at
# the kit matching this compiler -- MinGW-built Qt cannot link against MSVC.
# ---------------------------------------------------------------------------
if(NOT DEFINED QT_ROOT)
    set(QT_ROOT "C:/Qt/6.11.1/msvc2022_64" CACHE PATH "Qt kit matching this compiler")
endif()
list(APPEND CMAKE_PREFIX_PATH "${QT_ROOT}")

find_package(Qt6 6.5 COMPONENTS Core Gui Widgets PrintSupport QUIET)
if(NOT Qt6_FOUND)
    message(FATAL_ERROR
        "Qt6 not found at '${QT_ROOT}'.\n"
        "pcbview needs a Qt kit built with the SAME compiler as this project.\n"
        "This build uses ${CMAKE_CXX_COMPILER_ID}; MinGW-built Qt is ABI-incompatible with MSVC.\n"
        "\n"
        "To install the MSVC kit: run C:/Qt/MaintenanceTool.exe\n"
        "  -> Add or remove components -> Qt -> Qt 6.11.1 -> tick 'MSVC 2022 64-bit' -> install\n"
        "Then re-run cmake, or pass -DQT_ROOT=<path-to-kit>.")
endif()

# AUTOMOC runs Qt's meta-object compiler over anything with Q_OBJECT.
set(CMAKE_AUTOMOC ON)
message(STATUS "Qt ${Qt6_VERSION} from ${QT_ROOT} (dynamically linked, LGPL-3.0)")

# Shaders are compiled to SPIR-V at build time and embedded as C arrays, so the
# executable stays self-contained -- no .spv files to ship alongside it.
find_program(GLSLC NAMES glslc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" REQUIRED)

set(SHADER_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${SHADER_INCLUDE_DIR}/shaders)

function(pcbview_add_shader SHADER_NAME)
    set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${SHADER_NAME})
    set(OUT ${SHADER_INCLUDE_DIR}/shaders/${SHADER_NAME}.inc)
    add_custom_command(
        OUTPUT ${OUT}
        COMMAND ${GLSLC} -mfmt=c -O --target-env=vulkan1.3 ${SRC} -o ${OUT}
        DEPENDS ${SRC}
        COMMENT "Compiling ${SHADER_NAME} -> SPIR-V"
        VERBATIM
    )
    set(SHADER_OUTPUTS ${SHADER_OUTPUTS} ${OUT} PARENT_SCOPE)
endfunction()

pcbview_add_shader(board.vert)
pcbview_add_shader(board.frag)
pcbview_add_shader(board_rt.frag)
pcbview_add_shader(pathtrace.comp)
pcbview_add_shader(fullscreen.vert)
pcbview_add_shader(tonemap.frag)

add_executable(pcbview WIN32
    src/main.cpp
    src/io/sexpr.cpp
    src/io/obj_export.cpp
    src/io/kicad/kicad_importer.cpp
    src/io/gerber/gerber_parser.cpp
    src/io/gerber/gerber_project.cpp
    src/geom/tessellate.cpp
    ${MINIZ_SOURCES}
    src/text/stroke_text.cpp
    src/model/board.cpp
    src/model/validate.cpp
    src/render/common/device.cpp
    src/render/soft/preview.cpp
    src/render/vk/renderer.cpp
    src/app/vulkan_window.cpp
    src/app/main_window.cpp
    src/app/component_import.cpp
    src/app/theme.cpp
    src/app/viewer.cpp
    ${SHADER_OUTPUTS}
)

target_include_directories(pcbview PRIVATE
    src
    ${earcut_SOURCE_DIR}/include
    ${glm_SOURCE_DIR}
    ${miniz_SOURCE_DIR}
    ${cgltf_SOURCE_DIR}
    ${OIDN_DIR}/include
    ${SHADER_INCLUDE_DIR}
)

# miniz is third-party C; its warnings are not ours. MINIZ_EXPORT is normally
# defined in a generated miniz_export.h (produced by miniz's own CMake, which we
# bypass); define it empty so the header does not require that file.
set_source_files_properties(${MINIZ_SOURCES} PROPERTIES COMPILE_FLAGS
    $<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>)
# Do NOT define MINIZ_NO_STDIO: the file-based zip APIs (mz_zip_reader_init_file)
# are guarded by #ifndef, so even defining it to 0 disables them.
target_compile_definitions(pcbview PRIVATE MINIZ_EXPORT=)

# The generated miniz_export.h would normally sit beside the sources; provide a
# stub on the include path so #include "miniz_export.h" is satisfied.
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/miniz_export.h
     "#pragma once\n#ifndef MINIZ_EXPORT\n#define MINIZ_EXPORT\n#endif\n")

target_link_libraries(pcbview PRIVATE
    Vulkan::Vulkan
    Clipper2
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::PrintSupport
    ${OIDN_DIR}/lib/OpenImageDenoise.lib
)

# Stage the OIDN runtime beside the exe: the CPU device plus the CUDA (NVIDIA) and
# HIP (AMD) GPU devices. OIDN picks the fastest present at runtime -- a GPU when
# its driver is installed, else the CPU. The CUDA/HIP device DLLs need only the
# GPU driver (already on any machine with that GPU), not a toolkit. SYCL is not
# bundled (it drags in several more DLLs).
add_custom_command(TARGET pcbview POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${OIDN_DIR}/bin/OpenImageDenoise.dll
            ${OIDN_DIR}/bin/OpenImageDenoise_core.dll
            ${OIDN_DIR}/bin/OpenImageDenoise_device_cpu.dll
            ${OIDN_DIR}/bin/OpenImageDenoise_device_cuda.dll
            ${OIDN_DIR}/bin/OpenImageDenoise_device_hip.dll
            ${OIDN_DIR}/bin/tbb12.dll
            $<TARGET_FILE_DIR:pcbview>
    COMMENT "Staging Open Image Denoise runtime (CPU + CUDA + HIP) beside the executable"
    VERBATIM
)

if(MSVC)
    target_compile_options(pcbview PRIVATE /W4 /permissive-)
    # Windows subsystem: no console window hanging off the GUI.
    #
    # The CLI modes still print, because main() attaches to the PARENT console
    # when there is one (see attachParentConsole in main.cpp). Run it from a
    # terminal and output appears there; double-click it and no console is
    # created. /ENTRY:mainCRTStartup keeps main() as the entry point rather than
    # forcing WinMain.
    target_link_options(pcbview PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
else()
    target_compile_options(pcbview PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Deployment. LGPL-3.0 4(b): the GPL and LGPL texts must accompany every copy,
# and 4(a): a prominent notice. Copy them next to the exe so a zipped build
# directory is compliant on its own rather than relying on the repo being there.
# ---------------------------------------------------------------------------
add_custom_command(TARGET pcbview POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/LICENSES
            $<TARGET_FILE_DIR:pcbview>/LICENSES
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_CURRENT_SOURCE_DIR}/NOTICE.md
            $<TARGET_FILE_DIR:pcbview>/NOTICE.md
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
            $<TARGET_FILE_DIR:pcbview>/LICENSE
    COMMENT "Staging GPL-3.0 / LGPL-3.0 notices beside the executable"
    VERBATIM
)

# windeployqt copies the Qt DLLs and plugins beside the exe -- this is what makes
# the folder xcopy-deployable, and what keeps us on the dynamic-linking path.
find_program(WINDEPLOYQT NAMES windeployqt HINTS "${QT_ROOT}/bin")
if(WINDEPLOYQT)
    add_custom_target(deploy
        COMMAND ${WINDEPLOYQT} --release --no-translations --no-system-d3d-compiler
                --no-opengl-sw $<TARGET_FILE:pcbview>
        DEPENDS pcbview
        COMMENT "windeployqt: staging Qt DLLs for a portable folder"
        VERBATIM
    )
endif()
