cmake_minimum_required(VERSION 3.24)
project(ILARTS LANGUAGES CXX RC)

if(NOT WIN32)
  message(FATAL_ERROR "ILARTS must be built for Windows.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "ILARTS must be built for x64 (DCS is 64-bit). Use -A x64.")
endif()

# Static CRT: the DLL carries its own runtime and does not depend on whichever
# VC++ redistributable DCS happens to ship or load.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# Record only the PDB's file name in the binaries, not the build folder's full path (which holds the user name).
add_link_options(/PDBALTPATH:%_PDB%)

# The release version: SETTINGS > Installation > Version, the DLL's version string (dcs.log) and each file's
# version resource (src/version.rc). Not named ILARTS_VERSION: project(ILARTS) sets that one, empty.
# deploy/package.ps1 refuses a build whose version differs from its -Version:
#   cmake -S . -B build -A x64 "-DDCS_ILARTS_VERSION=1.0.27"   (the quotes: PowerShell splits it at the dot otherwise)
set(DCS_ILARTS_VERSION "0.0.0" CACHE STRING "DCS ILARTS version, numbers and dots (0.0.0: a development build)")
if(NOT DCS_ILARTS_VERSION MATCHES "^[0-9]+(\\.[0-9]+)*$")
  message(FATAL_ERROR "DCS_ILARTS_VERSION must be numbers and dots: ${DCS_ILARTS_VERSION}")
endif()
string(REPLACE "." ";" _ilarts_v "${DCS_ILARTS_VERSION}")
list(LENGTH _ilarts_v _ilarts_n)
if(_ilarts_n GREATER 4)
  message(FATAL_ERROR "DCS_ILARTS_VERSION has more than four numbers: ${DCS_ILARTS_VERSION}")
endif()
foreach(_p IN LISTS _ilarts_v)
  if(_p GREATER 65535)
    message(FATAL_ERROR "DCS_ILARTS_VERSION numbers must be 65535 or less: ${DCS_ILARTS_VERSION}")
  endif()
endforeach()
while(_ilarts_n LESS 4)
  list(APPEND _ilarts_v 0)
  math(EXPR _ilarts_n "${_ilarts_n} + 1")
endwhile()
string(REPLACE ";" "," ILARTS_VERSION_BIN "${_ilarts_v}")
set(ILARTS_VERSION_STR "${DCS_ILARTS_VERSION}")
configure_file(src/version.h.in ${CMAKE_BINARY_DIR}/generated/ilarts_version.h @ONLY)
include_directories(${CMAKE_BINARY_DIR}/generated)

add_library(ILARTS SHARED src/common.cpp src/diagnostic.cpp src/engine.cpp src/capture.cpp src/share.cpp src/render_callback.cpp src/mfd.cpp src/lso_camera.cpp src/plat_ui.cpp src/clouds_probe.cpp src/plat_origin.cpp src/own_canopy.cpp src/map_objects.cpp
    src/ship_trace.cpp src/memscan.cpp src/hullscan.cpp src/hull_refresh.cpp src/plat_cam.cpp src/compat.cpp
    src/extra_cams.cpp src/bench.cpp src/probe.cpp src/ILARTS.rc)
target_compile_features(ILARTS PRIVATE cxx_std_17)
target_compile_definitions(ILARTS PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(ILARTS PRIVATE /W4 /permissive- /EHsc /Zi)
# Generate a PDB in Release too, so crash dumps from inside DCS can be symbolised.
target_link_options(ILARTS PRIVATE /DEBUG /OPT:REF /OPT:ICF)
target_link_libraries(ILARTS PRIVATE version)
set_target_properties(ILARTS PROPERTIES OUTPUT_NAME "ILARTS")

# The viewer (ILARTS.exe): shows the shared PLAT / LSO screens, records video (runs outside DCS).
# viewer/ILARTS.rc: the icons (assets/, built by tools/make_icon.py).
add_executable(lso_viewer WIN32 viewer/lso_viewer.cpp viewer/ILARTS.rc)
target_compile_features(lso_viewer PRIVATE cxx_std_17)
target_compile_definitions(lso_viewer PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(lso_viewer PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(lso_viewer PRIVATE d3d11 dxgi d3dcompiler shell32 gdi32 user32 ole32 windowscodecs mfplat mf mfreadwrite mfuuid dwmapi ws2_32 oleaut32
    dcomp shlwapi advapi32 version ${CMAKE_SOURCE_DIR}/third_party/webview2/build/native/x64/WebView2LoaderStatic.lib)
# The console interface: WebView2 SDK headers (third_party/webview2, Microsoft.Web.WebView2 NuGet package, BSD
# licence) and the page itself, copied next to the executable.
target_include_directories(lso_viewer PRIVATE third_party/webview2/build/native/include)
add_custom_command(TARGET lso_viewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:lso_viewer>/ui/fonts
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/ui/index.html ${CMAKE_SOURCE_DIR}/ui/ilarts-console.js $<TARGET_FILE_DIR:lso_viewer>/ui
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/ui/fonts $<TARGET_FILE_DIR:lso_viewer>/ui/fonts)
set_target_properties(lso_viewer PROPERTIES OUTPUT_NAME "ILARTS")

# Spout senders (viewer/spout_out.h): the Spout2 SDK's SpoutDX (third_party/spout, BSD licence) and the wrapper,
# compiled without UNICODE as Spout expects.
add_library(spout_out STATIC viewer/spout_out.cpp third_party/spout/SpoutDX.cpp third_party/spout/SpoutDirectX.cpp
    third_party/spout/SpoutSenderNames.cpp third_party/spout/SpoutFrameCount.cpp third_party/spout/SpoutCopy.cpp
    third_party/spout/SpoutUtils.cpp third_party/spout/SpoutSharedMemory.cpp)
target_compile_features(spout_out PRIVATE cxx_std_17)
target_compile_definitions(spout_out PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(spout_out PRIVATE /W0 /EHsc)
target_include_directories(spout_out PRIVATE third_party/spout)
target_link_libraries(spout_out PRIVATE d3d11 dxgi winmm shell32 advapi32 version comctl32 shlwapi)
target_link_libraries(lso_viewer PRIVATE spout_out)

# The virtual cameras (ILARTSCam.dll, vcam/): a media source per camera for Windows' camera service; registered by
# "ILARTS.exe --install-cameras" (admin).
add_library(ilarts_cam SHARED vcam/ilarts_cam.cpp vcam/ilarts_cam.def vcam/ilarts_cam.rc)
target_compile_features(ilarts_cam PRIVATE cxx_std_17)
target_compile_definitions(ilarts_cam PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(ilarts_cam PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(ilarts_cam PRIVATE mfplat mfuuid mfsensorgroup ole32 advapi32)
set_target_properties(ilarts_cam PROPERTIES OUTPUT_NAME "ILARTSCam")

# Offline self-test of the sharing mechanism (no DCS).
add_executable(share_selftest tools/share_selftest.cpp)
target_compile_features(share_selftest PRIVATE cxx_std_17)
target_compile_definitions(share_selftest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(share_selftest PRIVATE d3d11 dxgi)

# Offline test of the own-canopy wrappers and patches (no DCS code runs; never deployed).
add_executable(own_canopy_test tools/own_canopy_test.cpp src/own_canopy.cpp)
target_compile_features(own_canopy_test PRIVATE cxx_std_17)
target_compile_definitions(own_canopy_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX OWN_CANOPY_TEST)
target_compile_options(own_canopy_test PRIVATE /W4 /permissive- /EHsc)

# Offline test host (never deployed to DCS).
add_executable(lsodiag_host host/lsodiag_host.cpp)
target_compile_features(lsodiag_host PRIVATE cxx_std_17)
target_compile_definitions(lsodiag_host PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(lsodiag_host PRIVATE /W4 /permissive- /EHsc)
