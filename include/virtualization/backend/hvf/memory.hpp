#pragma once
#include "virtualization/vm_context.hpp"

namespace virtualization {

/// Allocates guest RAM via shm_open + mmap and registers it with
/// hv_vm_map.  Also creates and configures the GICv3 interrupt
/// controller via hv_gic_create.
///
/// Ownership of the mapping is transferred to the VMContext and
/// released in VMContext::~VMContext via hv_vm_unmap + munmap.
class Memory {
public:
    [[nodiscard]] static bool init(VMContext& ctx);
};

} // namespace virtualization