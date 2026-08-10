cmake_minimum_required(VERSION 3.16)
project(onekvm_protocol_vnc LANGUAGES C CXX)

set(ONEKVM_EXTENSION_VERSION "0.0.0-dev" CACHE STRING "Extension version")
set(ONEKVM_EXTENSION_ARCH "unknown" CACHE STRING "Target architecture")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSON_C REQUIRED IMPORTED_TARGET json-c)

set(ONEKVM_LIBVNCSERVER_SOURCE "" CACHE PATH
    "Optional LibVNCServer source tree to build statically")
if(ONEKVM_LIBVNCSERVER_SOURCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(WITH_24BPP OFF CACHE BOOL "" FORCE)
  set(WITH_FFMPEG OFF CACHE BOOL "" FORCE)
  set(WITH_GCRYPT OFF CACHE BOOL "" FORCE)
  set(WITH_GNUTLS OFF CACHE BOOL "" FORCE)
  set(WITH_GTK OFF CACHE BOOL "" FORCE)
  set(WITH_IPv6 ON CACHE BOOL "" FORCE)
  set(WITH_JPEG ON CACHE BOOL "" FORCE)
  set(WITH_LIBSSHTUNNEL OFF CACHE BOOL "" FORCE)
  set(WITH_LZO OFF CACHE BOOL "" FORCE)
  set(WITH_OPENSSL OFF CACHE BOOL "" FORCE)
  set(WITH_PNG OFF CACHE BOOL "" FORCE)
  set(WITH_SASL OFF CACHE BOOL "" FORCE)
  set(WITH_SDL OFF CACHE BOOL "" FORCE)
  set(WITH_SYSTEMD OFF CACHE BOOL "" FORCE)
  set(WITH_TESTS OFF CACHE BOOL "" FORCE)
  set(WITH_THREADS OFF CACHE BOOL "" FORCE)
  set(WITH_TIGHTVNC_FILETRANSFER OFF CACHE BOOL "" FORCE)
  set(WITH_WEBSOCKETS OFF CACHE BOOL "" FORCE)
  set(WITH_XCB OFF CACHE BOOL "" FORCE)
  set(WITH_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(WITH_QT OFF CACHE BOOL "" FORCE)
  set(WITH_ZLIB ON CACHE BOOL "" FORCE)
  set(ONEKVM_SAVED_FIND_LIBRARY_SUFFIXES ${CMAKE_FIND_LIBRARY_SUFFIXES})
  set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
  add_subdirectory("${ONEKVM_LIBVNCSERVER_SOURCE}"
                   "${CMAKE_CURRENT_BINARY_DIR}/libvncserver" EXCLUDE_FROM_ALL)
  set(CMAKE_FIND_LIBRARY_SUFFIXES ${ONEKVM_SAVED_FIND_LIBRARY_SUFFIXES})
  set(ONEKVM_VNC_TARGET vncserver)
  set(ONEKVM_VNC_INCLUDE_DIRS
      "${ONEKVM_LIBVNCSERVER_SOURCE}/include"
      "${CMAKE_CURRENT_BINARY_DIR}/libvncserver/include")
else()
  pkg_check_modules(LIBVNCSERVER REQUIRED IMPORTED_TARGET libvncserver)
  set(ONEKVM_VNC_TARGET PkgConfig::LIBVNCSERVER)
endif()

add_executable(onekvm-protocol-vnc
  src/admin_hid.cpp
  src/config.cpp
  src/keymap.cpp
  src/main.cpp
  src/protocol_ipc.cpp
  src/vnc_server.cpp
)
target_include_directories(onekvm-protocol-vnc PRIVATE
  ${ONEKVM_VNC_INCLUDE_DIRS}
)
target_compile_options(onekvm-protocol-vnc PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(onekvm-protocol-vnc PRIVATE
  PkgConfig::JSON_C
  ${ONEKVM_VNC_TARGET}
  Threads::Threads
)

set(VERSION "${ONEKVM_EXTENSION_VERSION}")
set(ARCH "${ONEKVM_EXTENSION_ARCH}")
configure_file(manifest.json.in manifest.json @ONLY)

install(TARGETS onekvm-protocol-vnc RUNTIME DESTINATION bin)
install(FILES web/page.json web/vnc.svg DESTINATION web)
install(FILES NOTICE.icons NOTICE.openbmc DESTINATION .)
install(FILES LICENSES/Papirus-GPL-3.0.txt DESTINATION LICENSES)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/manifest.json" DESTINATION .)

option(ONEKVM_BUILD_TESTS "Build OneKVM VNC unit tests" OFF)
if(ONEKVM_BUILD_TESTS)
  enable_testing()
  add_executable(keymap-test tests/keymap_test.cpp src/keymap.cpp)
  target_include_directories(keymap-test PRIVATE src ${ONEKVM_VNC_INCLUDE_DIRS})
  target_compile_options(keymap-test PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(keymap-test PRIVATE ${ONEKVM_VNC_TARGET})
  add_test(NAME keymap-test COMMAND keymap-test)
endif()
