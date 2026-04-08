#pragma once
#include "virtualization/vm_context.hpp"

namespace virtualization {

/// Builds a minimal Flattened Device Tree blob and writes it at
/// kGpaDtbLoad in guest RAM.
/// Must be called after Memory::init() and after all devices have
/// been created on the VMContext.
class DTB {
public:
    [[nodiscard]] static bool generate(VMContext& ctx);
};

} // namespace virtualization