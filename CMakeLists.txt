cmake_minimum_required(VERSION 3.20)
project(NexusCap LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(NEXUSCAP_STREAMLINE_DIR "${CMAKE_SOURCE_DIR}/third_party/streamline-sdk" CACHE PATH "Streamline SDK root")
option(NEXUSCAP_WITH_STREAMLINE "Build with NVIDIA Streamline DLSS" ON)

set(NEXUSCAP_HAVE_STREAMLINE OFF)
if(NEXUSCAP_WITH_STREAMLINE)
  if(EXISTS "${NEXUSCAP_STREAMLINE_DIR}/include/sl.h" AND EXISTS "${NEXUSCAP_STREAMLINE_DIR}/bin/x64/sl.interposer.dll")
    set(NEXUSCAP_HAVE_STREAMLINE ON)
    message(STATUS "Streamline DLSS enabled: ${NEXUSCAP_STREAMLINE_DIR}")
  else()
    message(WARNING "Streamline SDK not found at ${NEXUSCAP_STREAMLINE_DIR}; building without DLSS")
    set(NEXUSCAP_WITH_STREAMLINE OFF)
  endif()
endif()

set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/third_party/imgui")

add_library(nexuscap_imgui STATIC
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
  ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(nexuscap_imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(nexuscap_imgui PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(nexuscap_imgui PUBLIC d3d11 dxgi user32 gdi32)

add_library(nexuscap_core STATIC
  src/clock.cpp
  src/config.cpp
  src/analyze_io.cpp
  src/capture_fake.cpp
  src/capture_dshow.cpp
  src/renderer_d3d11.cpp
  src/audio_wasapi.cpp
  src/enhance_passthrough.cpp
  src/enhance_dlss_streamline.cpp
  src/mvec_optical_flow.cpp
  src/lossless_fg.cpp
  src/settings_store.cpp
  src/session_report.cpp
  src/dlss5_bridge.cpp
  src/lobby_imgui.cpp
  src/system_metrics.cpp
)
target_include_directories(nexuscap_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(nexuscap_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(nexuscap_core PUBLIC
  nexuscap_imgui
  d3d11 dxgi d3dcompiler strmiids ole32 oleaut32 user32 gdi32 winmm uuid avrt advapi32 pdh comdlg32 windowscodecs
)

if(NEXUSCAP_HAVE_STREAMLINE)
  target_include_directories(nexuscap_core PRIVATE "${NEXUSCAP_STREAMLINE_DIR}/include")
  target_compile_definitions(nexuscap_core PRIVATE NEXUSCAP_WITH_STREAMLINE=1)
  target_link_libraries(nexuscap_core PUBLIC "${NEXUSCAP_STREAMLINE_DIR}/lib/x64/sl.interposer.lib")
endif()

add_executable(NexusCap src/main.cpp resources/nexuscap.rc)
target_link_libraries(NexusCap PRIVATE nexuscap_core)
target_compile_definitions(NexusCap PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_include_directories(NexusCap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/resources)

# Release/instalador: sem janela preta de CMD (assusta usuario).
# Debug local: console ON pra ver printf/logs no terminal.
option(NEXUSCAP_FORCE_CONSOLE "Force console even in Release (local only)" OFF)
if(MSVC)
  if(NEXUSCAP_FORCE_CONSOLE)
    target_link_options(NexusCap PRIVATE /SUBSYSTEM:CONSOLE)
  else()
    target_link_options(NexusCap PRIVATE
      $<$<CONFIG:Debug>:/SUBSYSTEM:CONSOLE>
      $<$<NOT:$<CONFIG:Debug>>:/SUBSYSTEM:WINDOWS>
      $<$<NOT:$<CONFIG:Debug>>:/ENTRY:mainCRTStartup>)
  endif()
endif()


add_executable(nexuscap_tests tests/test_core.cpp)
target_include_directories(nexuscap_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(nexuscap_tests PRIVATE nexuscap_core)
target_compile_definitions(nexuscap_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
  "NEXUSCAP_SOURCE_DIR=R\"(${CMAKE_SOURCE_DIR})\"")

add_executable(nexuscap_probe tools/probe_capture.cpp)
target_link_libraries(nexuscap_probe PRIVATE nexuscap_core)
target_compile_definitions(nexuscap_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

enable_testing()
add_test(NAME unit COMMAND nexuscap_tests)
add_test(NAME smoke COMMAND NexusCap --smoke --frames 120 --log-dir ${CMAKE_BINARY_DIR}/logs-smoke)
add_test(NAME smoke_fg_off COMMAND NexusCap --smoke --fake-capture --fg-x 1 --frames 60 --log-dir ${CMAKE_BINARY_DIR}/logs-smoke-fg)
add_test(NAME smoke_no_lobby COMMAND NexusCap --smoke --no-lobby --fg-x 1 --frames 60 --log-dir ${CMAKE_BINARY_DIR}/logs-smoke-nolobby)
add_test(NAME selftest_fg COMMAND NexusCap --selftest-fg --fake-capture --windowed --log-dir ${CMAKE_BINARY_DIR}/logs-selftest-fg)
set_tests_properties(smoke PROPERTIES TIMEOUT 30)
set_tests_properties(smoke_fg_off PROPERTIES TIMEOUT 30)
set_tests_properties(smoke_no_lobby PROPERTIES TIMEOUT 30)
set_tests_properties(selftest_fg PROPERTIES TIMEOUT 45)

# Copy Streamline runtime next to NexusCap.exe
if(NEXUSCAP_HAVE_STREAMLINE)
  set(_sl_bin "${NEXUSCAP_STREAMLINE_DIR}/bin/x64")
  set(_sl_dlls
    sl.interposer.dll
    sl.common.dll
    sl.dlss.dll
    sl.pcl.dll
    nvngx_dlss.dll
  )
  foreach(_dll ${_sl_dlls})
    add_custom_command(TARGET NexusCap POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_sl_bin}/${_dll}"
        "$<TARGET_FILE_DIR:NexusCap>/${_dll}"
      COMMENT "Copy ${_dll}"
    )
  endforeach()
endif()

install(TARGETS NexusCap RUNTIME DESTINATION bin)

# Brand logo next to the exe for lobby / window icon.
add_custom_command(TARGET NexusCap POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:NexusCap>/assets"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/assets/logo.png"
    "$<TARGET_FILE_DIR:NexusCap>/assets/logo.png"
  COMMENT "Copy assets/logo.png"
)
