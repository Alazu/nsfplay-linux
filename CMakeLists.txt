cmake_minimum_required(VERSION 3.16)
project(nsfplay VERSION 3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(ALSA REQUIRED)
find_package(Threads REQUIRED)

file(GLOB_RECURSE XGM_CPP xgm/devices/*.cpp xgm/player/*.cpp)
set(CORE_SRC
  ${XGM_CPP}
  xgm/fileutil.cpp
  xgm/devices/Sound/legacy/emu2149.c
  xgm/devices/Sound/legacy/emu2212.c
  xgm/devices/Sound/legacy/emu2413.c
  vcm/group.cpp
  vcm/value.cpp)
add_library(xgmcore STATIC ${CORE_SRC})
target_compile_options(xgmcore PRIVATE -Wno-deprecated-declarations)
set_target_properties(xgmcore PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(nsfplay linux/main.cpp linux/engine.cpp linux/res/nsfplay.qrc)
target_compile_definitions(nsfplay PRIVATE NSFPLAY_LINUX_VERSION="${PROJECT_VERSION}")
target_link_libraries(nsfplay PRIVATE xgmcore Qt6::Widgets ALSA::ALSA Threads::Threads)

# command line tools from contrib
add_executable(nsf2wav contrib/nsf2wav.cpp)
target_link_libraries(nsf2wav PRIVATE xgmcore)
add_executable(nsfmeta contrib/nsfmeta.cpp)
target_include_directories(nsfmeta PRIVATE contrib)
target_link_libraries(nsfmeta PRIVATE xgmcore)

install(TARGETS nsfplay nsf2wav nsfmeta RUNTIME DESTINATION bin)
install(FILES linux/res/nsfplay.desktop DESTINATION share/applications)
install(FILES linux/res/nsfplay.png DESTINATION share/icons/hicolor/256x256/apps)
