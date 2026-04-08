#pragma once
// Internal translation-unit boundary header.
// Not part of the public include tree.

namespace virtualization {
class VCpu;

struct ExitHandler {
    // Returns false → stop this vCPU thread (unrecoverable fault or halt).
    [[nodiscard]] static bool handle(VCpu& vcpu);
};

} // namespace virtualization