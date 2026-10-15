cmake_minimum_required(VERSION 3.24)
project(AIVideoCuttyWindows VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
  # Bundle the small CRT in the executable, not a separately installed runtime.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

enable_testing()
add_executable(playback_rules_tests tests/playback_rules_tests.cpp)
target_include_directories(playback_rules_tests PRIVATE src)
add_test(NAME playback_rules COMMAND playback_rules_tests)

if(WIN32)
  enable_language(RC)
  add_executable(AIVideoCutty WIN32 src/main.cpp src/app.rc src/AIVideoCutty.ico src/app.manifest)
  target_compile_definitions(AIVideoCutty PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    WINVER=0x0A00 _WIN32_WINNT=0x0A00)
  target_link_libraries(AIVideoCutty PRIVATE mfplat mf mfuuid mfreadwrite d3d11 dxgi
    windowscodecs ole32 oleaut32 comctl32 shell32 shlwapi propsys imm32)
  if(MSVC)
    target_compile_options(AIVideoCutty PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(playback_rules_tests PRIVATE /W4 /permissive- /utf-8)
    target_link_options(AIVideoCutty PRIVATE /MANIFEST:EMBED
      "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/app.manifest")
  endif()
  install(TARGETS AIVideoCutty RUNTIME DESTINATION .)
  install(FILES README.md DESTINATION . RENAME Windows-使用说明.md)
endif()
