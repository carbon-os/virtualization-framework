#pragma once
#include "virtualization/vm_context.hpp"
#include <string_view>

namespace virtualization {

/// Loads an arm64 kernel Image and an optional initrd into guest RAM.
/// Validates the arm64 boot magic, derives the kernel entry GPA from
/// the image header's text_offset, and registers everything with ctx.
class Loader {
public:
    /// Load the uncompressed kernel Image.  Sets ctx.kernel_entry().
    [[nodiscard]] static bool load_kernel(VMContext& ctx, std::string_view path);

    /// Load the initrd/initramfs.  Sets ctx.initrd_gpa/size().
    [[nodiscard]] static bool load_initrd(VMContext& ctx, std::string_view path);
};

} // namespace virtualization