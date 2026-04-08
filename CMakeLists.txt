cmake_minimum_required(VERSION 3.25)

project(virtualization-framework
    VERSION      0.1.0
    DESCRIPTION  "Cross-platform hardware-accelerated virtualization framework"
    LANGUAGES    CXX
)

# ── Language standard ──────────────────────────────────────────────────────────

set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# ── Build type default ─────────────────────────────────────────────────────────

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

# ── Platform / backend detection ───────────────────────────────────────────────

if(APPLE)
    set(VIRT_BACKEND "hvf" CACHE STRING "Hypervisor backend (hvf)")
    set(VIRT_BACKEND_VALUES "hvf")
elseif(UNIX)
    set(VIRT_BACKEND "kvm" CACHE STRING "Hypervisor backend (kvm)")
    set(VIRT_BACKEND_VALUES "kvm")
elseif(WIN32)
    set(VIRT_BACKEND "whp" CACHE STRING "Hypervisor backend (whp)")
    set(VIRT_BACKEND_VALUES "whp")
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

set_property(CACHE VIRT_BACKEND PROPERTY STRINGS ${VIRT_BACKEND_VALUES})

# ── Architecture detection ─────────────────────────────────────────────────────

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)")
    set(VIRT_ARCH "arm64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)")
    set(VIRT_ARCH "x86_64")
else()
    message(FATAL_ERROR "Unsupported architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

message(STATUS "Backend : ${VIRT_BACKEND}")
message(STATUS "Arch    : ${VIRT_ARCH}")

# ── Options ────────────────────────────────────────────────────────────────────

option(VIRT_BUILD_SHARED        "Build as a shared library"          OFF)
option(VIRT_BUILD_SAMPLES       "Build sample programs"              ON)
option(VIRT_ENABLE_NETWORK      "Build userspace network stack"      ON)
option(VIRT_WARNINGS_AS_ERRORS  "Treat warnings as errors"           OFF)

# ── Sources ────────────────────────────────────────────────────────────────────

set(VIRT_SOURCES
    # VirtIO devices
    src/virtio/block.cpp
    src/virtio/console.cpp
    src/virtio/net.cpp
    src/virtio/gpu.cpp
    src/virtio/input.cpp      # <-- Added virtio-input here!

    # Core
    src/core/vm.cpp
    src/core/loader.cpp
    src/core/logger.cpp

    # Backend — selected at configure time
    src/backend/${VIRT_BACKEND}/vcpu.cpp
    src/backend/${VIRT_BACKEND}/memory.cpp
    src/backend/${VIRT_BACKEND}/exit_handler.cpp
)

if(VIRT_ARCH STREQUAL "arm64")
    list(APPEND VIRT_SOURCES
        src/firmware/arm64/dtb.cpp
        src/firmware/arm64/gic.cpp
        src/firmware/arm64/psci.cpp
    )
elseif(VIRT_ARCH STREQUAL "x86_64")
    list(APPEND VIRT_SOURCES
        src/firmware/x86_64/acpi.cpp
        src/firmware/x86_64/apic.cpp
        src/firmware/x86_64/msr.cpp
    )
endif()

if(VIRT_ENABLE_NETWORK)
    file(GLOB NETWORK_SOURCES CONFIGURE_DEPENDS src/network/*.cpp)
    list(APPEND VIRT_SOURCES ${NETWORK_SOURCES})
endif()

# ── Library target ─────────────────────────────────────────────────────────────

if(VIRT_BUILD_SHARED)
    add_library(virtualization SHARED ${VIRT_SOURCES})
else()
    add_library(virtualization STATIC ${VIRT_SOURCES})
endif()

add_library(virtualization::virtualization ALIAS virtualization)

# ── Include directories ────────────────────────────────────────────────────────

target_include_directories(virtualization
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ── Compile definitions ────────────────────────────────────────────────────────

string(TOUPPER "${VIRT_BACKEND}" VIRT_BACKEND_UPPER)
string(TOUPPER "${VIRT_ARCH}"    VIRT_ARCH_UPPER)

target_compile_definitions(virtualization
    PUBLIC
        VIRT_BACKEND_${VIRT_BACKEND_UPPER}
        VIRT_ARCH_${VIRT_ARCH_UPPER}
    PRIVATE
        VIRT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        VIRT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        VIRT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

# ── Compile options ────────────────────────────────────────────────────────────

target_compile_options(virtualization PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion>
    $<$<CXX_COMPILER_ID:MSVC>:
        /W4 /permissive->
)

if(VIRT_WARNINGS_AS_ERRORS)
    target_compile_options(virtualization PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>
        $<$<CXX_COMPILER_ID:MSVC>:/WX>
    )
endif()

# ── Platform-specific linker flags ─────────────────────────────────────────────

if(APPLE)
    target_link_libraries(virtualization PUBLIC "-framework Hypervisor")
elseif(UNIX)
    target_link_libraries(virtualization PUBLIC pthread)
elseif(WIN32)
    target_link_libraries(virtualization PUBLIC WinHvPlatform)
endif()

# ── Samples ────────────────────────────────────────────────────────────────────

if(VIRT_BUILD_SAMPLES)
    add_subdirectory(samples)
endif()