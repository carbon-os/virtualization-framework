#include "virtualization/firmware/arm64/psci.hpp"
#include "virtualization/backend/hvf/vcpu.hpp"
#include "virtualization/vm_context.hpp"
#include "virtualization/logger.hpp"
#include <Hypervisor/Hypervisor.h>

// ── Private helpers ───────────────────────────────────────────────────────────
namespace {

inline hv_reg_t xreg_id(uint32_t n) noexcept {
    return static_cast<hv_reg_t>(HV_REG_X0 + n);
}

uint64_t get_xreg(virtualization::VCpu& vcpu, uint32_t n) noexcept {
    if (n == 31) return 0;
    uint64_t v = 0;
    hv_vcpu_get_reg(vcpu.handle(), xreg_id(n), &v);
    return v;
}

void set_xreg(virtualization::VCpu& vcpu, uint32_t n, uint64_t val) noexcept {
    if (n == 31) return;
    hv_vcpu_set_reg(vcpu.handle(), xreg_id(n), val);
}

} // namespace

namespace virtualization {

// Returns false → caller should stop this vCPU thread (CPU_OFF / SYSTEM_OFF).
bool psci_dispatch(VCpu& vcpu) {
    VMContext&    ctx = vcpu.ctx_ref();
    const auto fn = static_cast<uint32_t>(get_xreg(vcpu, 0));

    switch (fn) {

        case kPsciVersion:
            set_xreg(vcpu, 0, 0x00010000ULL); // PSCI 1.0
            break;

        case kPsciCpuOn: {
            const uint32_t target =
                static_cast<uint32_t>(get_xreg(vcpu, 1) & 0xFF);
            const uint64_t entry  = get_xreg(vcpu, 2);
            const uint64_t ctx_id = get_xreg(vcpu, 3);
            if (target >= ctx.vcpu_count()) {
                set_xreg(vcpu, 0, kPsciRetInvalidParams);
                break;
            }
            VCpu& target_vcpu = ctx.vcpu(target);
            target_vcpu.boot_pc.store(entry,  std::memory_order_relaxed);
            target_vcpu.boot_x0.store(ctx_id, std::memory_order_relaxed);
            target_vcpu.boot_pending.store(true, std::memory_order_release);
            set_xreg(vcpu, 0, kPsciRetSuccess);
            break;
        }

        case kPsciCpuOff:
            logger::log("[psci] CPU %u OFF\n", vcpu.index());
            return false;

        case kPsciSystemOff:
        case kPsciSystemReset:
            logger::log("[psci] SYSTEM_%s\n",
                        fn == kPsciSystemOff ? "OFF" : "RESET");
            ctx.stop();
            return false;

        case kPsciMigrateInfo:
            // No trusted-OS migration needed — return NOT_PRESENT (2).
            set_xreg(vcpu, 0, 2);
            break;

        case kPsciCpuSuspend:
            // Treat as WFI — just return success; the vCPU will re-enter.
            set_xreg(vcpu, 0, kPsciRetSuccess);
            break;

        default:
            logger::log("[psci] vcpu%u unknown fn 0x%08x\n", vcpu.index(), fn);
            set_xreg(vcpu, 0, kPsciRetNotSupported);
            break;
    }
    return true;
}

} // namespace virtualization