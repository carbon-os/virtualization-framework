# ── sample-vm ──────────────────────────────────────────────────────────────────
#
#  Standalone CMake scope for all sample programs.
#  Consumed by the top-level project via add_subdirectory(samples).
#  The virtualization::virtualization target is already defined by then.

# ── Common signing helper ──────────────────────────────────────────────────────
# Pulled out so each target below can call it without repeating the
# codesign boilerplate.  Pass the target name as the only argument.

macro(sign_with_hypervisor_entitlements TARGET_NAME)
    if(APPLE AND EXISTS "${VIRT_ENTITLEMENTS}")
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CODESIGN}
                --sign -
                --entitlements "${VIRT_ENTITLEMENTS}"
                --force
                "$<TARGET_FILE:${TARGET_NAME}>"
            COMMENT "Signing ${TARGET_NAME} with Hypervisor entitlements"
            VERBATIM
        )
    endif()
endmacro()

# ── Entitlements / codesign (resolved once, shared by all targets) ─────────────

if(APPLE)
    set(VIRT_ENTITLEMENTS
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/entitlements.plist"
        CACHE FILEPATH "Path to the entitlements plist used for code signing"
    )

    if(NOT EXISTS "${VIRT_ENTITLEMENTS}")
        message(WARNING
            "Entitlements file not found: ${VIRT_ENTITLEMENTS}\n"
            "Sample binaries will NOT be signed — Hypervisor.framework calls will fail.")
    else()
        find_program(CODESIGN codesign REQUIRED)
        message(STATUS "Samples will be signed with: ${VIRT_ENTITLEMENTS}")
    endif()
endif()

# ── sample-vm ──────────────────────────────────────────────────────────────────
# Minimal headless boot: kernel + disk + NAT network + virtio console.

add_executable(sample-vm main.cpp)

target_link_libraries(sample-vm PRIVATE virtualization::virtualization)

target_compile_options(sample-vm PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:
        -Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:
        /W4 /permissive->
)

sign_with_hypervisor_entitlements(sample-vm)

# ── sample-vm-display-mac ──────────────────────────────────────────────────────
# Boots a Linux arm64 guest with a virtio-gpu 2D framebuffer and renders
# each frame into a native NSWindow via CoreGraphics / CALayer.
# macOS only — silently skipped on other platforms.

if(APPLE)
    add_executable(sample-vm-display-mac main_display_mac.mm)

    target_link_libraries(sample-vm-display-mac PRIVATE
        virtualization::virtualization
        "-framework Cocoa"
        "-framework QuartzCore"
    )

    target_compile_options(sample-vm-display-mac PRIVATE
        -Wall -Wextra -Wpedantic
        -fobjc-arc
    )

    sign_with_hypervisor_entitlements(sample-vm-display-mac)
endif()