cmake_minimum_required(VERSION 3.16)

project(ravensight
    VERSION 0.2.0
    DESCRIPTION "Official C++ SDK for Ravensight player analytics"
    HOMEPAGE_URL "https://ravensight.io"
    LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(RAVENSIGHT_MAIN_PROJECT ON)
else()
    set(RAVENSIGHT_MAIN_PROJECT OFF)
endif()

option(RAVENSIGHT_BUILD_TESTS "Build the ravensight unit tests" ${RAVENSIGHT_MAIN_PROJECT})
option(RAVENSIGHT_BUILD_EXAMPLES "Build the ravensight examples" ${RAVENSIGHT_MAIN_PROJECT})

# BUILD_SHARED_LIBS decides static vs shared, as usual for CMake.
add_library(ravensight
    src/client.cpp
    src/curl_transport.cpp
    src/json.cpp
    src/queue.cpp
    src/storage.cpp
    src/transport.cpp)
add_library(ravensight::ravensight ALIAS ravensight)

target_compile_features(ravensight PUBLIC cxx_std_17)
set_target_properties(ravensight PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    POSITION_INDEPENDENT_CODE ON)

target_include_directories(ravensight PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
target_link_libraries(ravensight
    PRIVATE CURL::libcurl
    PUBLIC Threads::Threads)
if(WIN32)
    # BCryptGenRandom, the device id random source on Windows.
    target_link_libraries(ravensight PRIVATE bcrypt)
endif()

if(MSVC)
    target_compile_options(ravensight PRIVATE /W4)
else()
    target_compile_options(ravensight PRIVATE -Wall -Wextra)
endif()

# --- Tests -------------------------------------------------------------------

if(RAVENSIGHT_BUILD_TESTS)
    enable_testing()
    add_executable(ravensight_tests
        tests/test_main.cpp
        tests/json_tests.cpp
        tests/queue_tests.cpp
        tests/client_tests.cpp
        tests/storage_tests.cpp
        tests/conformance_tests.cpp)
    target_link_libraries(ravensight_tests PRIVATE ravensight::ravensight)
    target_include_directories(ravensight_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
    if(MSVC)
        target_compile_options(ravensight_tests PRIVATE /W4)
    else()
        target_compile_options(ravensight_tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME ravensight_tests COMMAND ravensight_tests)
endif()

# --- Examples ----------------------------------------------------------------

if(RAVENSIGHT_BUILD_EXAMPLES)
    add_executable(ravensight_example examples/track_events.cpp)
    target_link_libraries(ravensight_example PRIVATE ravensight::ravensight)
endif()

# --- Install -----------------------------------------------------------------

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS ravensight
    EXPORT ravensightTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY include/ravensight DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT ravensightTargets
    NAMESPACE ravensight::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ravensight)

configure_package_config_file(
    cmake/ravensightConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ravensightConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ravensight)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/ravensightConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/ravensightConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/ravensightConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ravensight)
