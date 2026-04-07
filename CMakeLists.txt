cmake_minimum_required(VERSION 3.21)
project(pitch_bend_glide VERSION 0.1.0 LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(FetchContent)

# ── CLAP SDK (headers only) ───────────────────────────────────────────────────
FetchContent_Declare(clap
	GIT_REPOSITORY https://github.com/free-audio/clap.git
	GIT_TAG        1.2.2
	GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(clap)

# ── Pugl (windowing + input; vendored in third_party/pugl) ───────────────────
# Use the stub backend — no rendering context; Metal is attached separately.
set(PUGL_DIR ${CMAKE_SOURCE_DIR}/third_party/pugl)

add_library(pugl STATIC
	${PUGL_DIR}/src/common.c
	${PUGL_DIR}/src/internal.c
	${PUGL_DIR}/src/mac.m
	${PUGL_DIR}/src/mac_stub.m
)
target_include_directories(pugl PUBLIC
	${PUGL_DIR}/include
)
target_include_directories(pugl PRIVATE
	${PUGL_DIR}/src
)
target_link_libraries(pugl PUBLIC
	"-framework Cocoa"
)
# Pugl's .m files are plain Objective-C; compile without ARC.
set_source_files_properties(
	${PUGL_DIR}/src/mac.m
	${PUGL_DIR}/src/mac_stub.m
	PROPERTIES COMPILE_FLAGS "-fno-objc-arc"
)

# pugl_mac_metal needs access to Pugl private headers (internal.h, mac.h, stub.h)
# and must be compiled without ARC to match the rest of Pugl.
add_library(pugl_metal STATIC
	src/pugl_mac_metal.m
)
target_include_directories(pugl_metal PRIVATE
	${PUGL_DIR}/include
	${PUGL_DIR}/src
	src
)
target_link_libraries(pugl_metal PUBLIC
	"-framework Metal"
	"-framework QuartzCore"
	"-framework Cocoa"
)
set_source_files_properties(src/pugl_mac_metal.m
	PROPERTIES COMPILE_FLAGS "-fno-objc-arc"
)

# ── Dear ImGui (vendored in imgui/) ──────────────────────────────────────────
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/imgui)

add_library(imgui_lib STATIC
	${IMGUI_DIR}/imgui.cpp
	${IMGUI_DIR}/imgui_draw.cpp
	${IMGUI_DIR}/imgui_widgets.cpp
	${IMGUI_DIR}/imgui_tables.cpp
	${IMGUI_DIR}/backends/imgui_impl_metal.mm
)
target_include_directories(imgui_lib PUBLIC
	${IMGUI_DIR}
	${IMGUI_DIR}/backends
)
target_compile_options(imgui_lib PRIVATE -fobjc-arc)
target_link_libraries(imgui_lib PUBLIC
	"-framework Metal"
	"-framework MetalKit"
	"-framework QuartzCore"
)

# ── Plugin ────────────────────────────────────────────────────────────────────
add_library(pitch_bend_glide MODULE
	src/plugin.cpp
	src/glide_engine.cpp
	src/event_buffer.cpp
	src/imgui_impl_pugl.cpp
	src/gui.mm
)
target_include_directories(pitch_bend_glide PRIVATE
	src
	${clap_SOURCE_DIR}/include
)
target_link_libraries(pitch_bend_glide PRIVATE
	imgui_lib
	pugl
	pugl_metal
	"-framework Cocoa"
	"-framework Foundation"
)
# gui.mm must not use ARC — PuglMetalContext holds raw ObjC refs managed by pugl_mac_metal.m
set_source_files_properties(src/gui.mm PROPERTIES
	COMPILE_FLAGS "-fno-objc-arc"
)
target_compile_options(pitch_bend_glide PRIVATE
	-Wall
	-Wextra
	-Wno-unused-parameter
)

# ── macOS .clap bundle ────────────────────────────────────────────────────────
if(APPLE)
	set_target_properties(pitch_bend_glide PROPERTIES
		BUNDLE                              TRUE
		BUNDLE_EXTENSION                    clap
		MACOSX_BUNDLE_GUI_IDENTIFIER        com.pitchbend.glide
		MACOSX_BUNDLE_BUNDLE_NAME           "Pitch Bend Glide"
		MACOSX_BUNDLE_BUNDLE_VERSION        ${PROJECT_VERSION}
		MACOSX_BUNDLE_SHORT_VERSION_STRING  ${PROJECT_VERSION}
		MACOSX_BUNDLE_INFO_PLIST            ${CMAKE_SOURCE_DIR}/res/Info.plist.in
		OUTPUT_NAME                         "Pitch Bend Glide"
	)
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
enable_testing()

add_executable(test_glide_engine
	tests/test_glide_engine.cpp
	src/glide_engine.cpp
)
target_include_directories(test_glide_engine PRIVATE src)
target_compile_options(test_glide_engine PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_test(NAME glide_engine COMMAND test_glide_engine)

add_executable(test_legato_routing
	tests/test_legato_routing.cpp
	src/plugin.cpp
	src/glide_engine.cpp
	src/event_buffer.cpp
)
target_include_directories(test_legato_routing PRIVATE
	src
	${clap_SOURCE_DIR}/include
)
target_compile_options(test_legato_routing PRIVATE -Wall -Wextra -Wno-unused-parameter)
# Exclude GUI / ObjC symbols — the test doesn't need them.
target_compile_definitions(test_legato_routing PRIVATE PITCH_BEND_NO_GUI=1)

add_test(NAME legato_routing COMMAND test_legato_routing)

# ── Install to user CLAP folder ───────────────────────────────────────────────
if(APPLE)
	install(TARGETS pitch_bend_glide
		DESTINATION "$ENV{HOME}/Library/Audio/Plug-Ins/CLAP"
	)
endif()
