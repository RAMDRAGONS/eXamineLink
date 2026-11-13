cmake_minimum_required(VERSION 3.21)
project(eXamineLink VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

foreach(dep zstd/lib/zstd.h)
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/external/${dep})
    message(FATAL_ERROR "external/${dep} is missing; run: git submodule update --init")
  endif()
endforeach()

# Zstandard is built from source so the encoder and decoder match the
# submodule's version exactly; the assembly Huffman path is left out to keep
# the build portable.
set(ZSTD_LIB ${CMAKE_CURRENT_SOURCE_DIR}/external/zstd/lib)
file(GLOB XL_ZSTD_SOURCES
  ${ZSTD_LIB}/common/*.c
  ${ZSTD_LIB}/compress/*.c
  ${ZSTD_LIB}/decompress/*.c)
add_library(xl_zstd STATIC ${XL_ZSTD_SOURCES})
target_compile_definitions(xl_zstd PRIVATE ZSTD_DISABLE_ASM ZSTD_MULTITHREAD=0)
target_include_directories(xl_zstd SYSTEM PUBLIC ${ZSTD_LIB})
set_target_properties(xl_zstd PROPERTIES POSITION_INDEPENDENT_CODE ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets)

# Everything that reads, edits and writes databases; only QtCore (for XML), so
# the command-line round-trip checks exercise exactly the code the editor uses.
add_library(xl_core STATIC
  src/core/text.cpp
  src/codec/yaz.cpp
  src/codec/sarc.cpp
  src/codec/zstd.cpp
  src/codec/layers.cpp
  src/model/node.cpp
  src/model/xlink_schema.cpp
  src/model/eslink_schema.cpp
  src/model/elink0_schema.cpp
  src/model/document.cpp
  src/model/edit.cpp
  src/model/value_text.cpp
  src/model/xml_io.cpp
  src/model/bridge.cpp
  src/model/convert.cpp
  src/model/validate.cpp
  src/model/factory.cpp
  src/formats/string_pool.cpp
  src/formats/xlink_words.cpp
  src/formats/xlink_v19.cpp
  src/formats/xlink_modern.cpp
  src/formats/eslink.cpp
  src/formats/elink0.cpp
)
target_include_directories(xl_core PUBLIC src)
target_link_libraries(xl_core PUBLIC xl_zstd Qt6::Core)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(xl_core PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endif()
if(MSVC)
  target_compile_options(xl_core PRIVATE /utf-8 /W3)
endif()

add_library(xl_ui STATIC
  src/ui/mainwindow.cpp
  src/ui/treemodel.cpp
  src/ui/fieldtable.cpp
  src/ui/importusersdialog.cpp
  src/ui/saveasdialog.cpp
)
target_link_libraries(xl_ui PUBLIC xl_core Qt6::Widgets)
target_compile_definitions(xl_ui PRIVATE XL_VERSION="${PROJECT_VERSION}")

add_executable(examinelink WIN32 src/main.cpp)
target_link_libraries(examinelink PRIVATE xl_ui)
if(MSVC)
  target_compile_options(examinelink PRIVATE /utf-8)
endif()

add_executable(examinelink-cli src/cli/main.cpp)
target_link_libraries(examinelink-cli PRIVATE xl_core)

# Self-contained install for release archives: Qt's libraries and plugins,
# and on Windows the MSVC runtime, are copied beside the executables. Off by
# default so system installs keep using the system's Qt.
option(XL_DEPLOY_QT "Bundle Qt and the compiler runtime with the installed executables" OFF)
if(XL_DEPLOY_QT)
  if(Qt6_VERSION VERSION_LESS 6.5)
    message(FATAL_ERROR "XL_DEPLOY_QT needs Qt 6.5 or later, found ${Qt6_VERSION}")
  endif()
  if(UNIX AND NOT APPLE)
    set_target_properties(examinelink examinelink-cli PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
  endif()
  if(MSVC)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION bin)
    include(InstallRequiredSystemLibraries)
  endif()
endif()

install(TARGETS examinelink examinelink-cli RUNTIME DESTINATION bin)

if(XL_DEPLOY_QT)
  set(xl_deploy_filter "")
  if(UNIX AND NOT APPLE)
    # Only Qt and the ICU it was built with; libraries every desktop has,
    # glibc above all, must come from the user's system.
    set(xl_deploy_filter POST_INCLUDE_REGEXES "/libQt6[^/]*[.]so" "/libicu[^/]*[.]so" POST_EXCLUDE_REGEXES ".*")
  endif()
  qt_generate_deploy_app_script(TARGET examinelink OUTPUT_SCRIPT xl_deploy_script NO_TRANSLATIONS NO_COMPILER_RUNTIME
                                ${xl_deploy_filter})
  install(SCRIPT ${xl_deploy_script})
endif()

enable_testing()
# Round trips run against real databases named in XL_SAMPLES, a
# semicolon-separated list of paths; none are bundled with the sources.
set(XL_SAMPLES "" CACHE STRING "Database files the round-trip tests load")
set(sample_index 0)
foreach(sample IN LISTS XL_SAMPLES)
  # Numbered: different games ship databases under the same file name.
  math(EXPR sample_index "${sample_index} + 1")
  get_filename_component(sample_name ${sample} NAME)
  set(tag "${sample_index}-${sample_name}")
  add_test(NAME check:${tag} COMMAND examinelink-cli check ${sample})
  add_test(NAME ui:${tag} COMMAND examinelink-uitest ${sample} ${CMAKE_CURRENT_BINARY_DIR}/ui-${tag}.png)
  set_tests_properties(ui:${tag} PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
endforeach()

# The same database shipped for two platforms, as FILE|REFERENCE pairs:
# converting the first into the second's byte order must reproduce it.
set(XL_PAIRS "" CACHE STRING "Database pairs the cross-platform tests compare")
set(pair_index 0)
foreach(pair IN LISTS XL_PAIRS)
  math(EXPR pair_index "${pair_index} + 1")
  string(REPLACE "|" ";" files "${pair}")
  list(GET files 0 from)
  list(GET files 1 to)
  get_filename_component(pair_name ${to} NAME)
  add_test(NAME matches:${pair_index}-${pair_name} COMMAND examinelink-cli matches ${from} ${to})
endforeach()

# Users carried between databases of one link system, as TARGET|SOURCE
# pairs: every user of SOURCE must import into TARGET, read back, and, from
# EffectLink or SoundLink, return playing the same.
set(XL_BRIDGES "" CACHE STRING "Database pairs the user import tests carry users between")
set(bridge_index 0)
foreach(bridge IN LISTS XL_BRIDGES)
  math(EXPR bridge_index "${bridge_index} + 1")
  string(REPLACE "|" ";" files "${bridge}")
  list(GET files 0 into)
  list(GET files 1 from)
  get_filename_component(bridge_name ${from} NAME)
  add_test(NAME bridge:${bridge_index}-${bridge_name} COMMAND examinelink-cli bridge-check ${into} ${from})
endforeach()

# Drives the editor without a display: opens a database, selects and edits
# an entry, undoes, and saves screenshots for inspection.
add_executable(examinelink-uitest tests/ui_smoke.cpp)
target_link_libraries(examinelink-uitest PRIVATE xl_ui)

