#include "virtualization/backend/hvf/vcpu.hpp"
#include "exit_handler.hpp"
#include "virtualization/vm_context.hpp"
#include "virtualization/logger.hpp"
#include <Hypervisor/Hypervisor.h>
#include <sched.h>

namespace virtualization {

VCpu::VCpu(VMContext& ctx, uint32_t index) noexcept
    : ctx_(ctx), index_(index) {}

VCpu::~VCpu() { join(); }

void VCpu::spawn() {
    thread_ = std::thread(&VCpu::thread_func, this);
}

void VCpu::join() {
    if (thread_.joinable()) thread_.join();
}

bool VCpu::setup_regs(uint64_t pc, uint64_t x0) {
    const hv_vcpu_t v = handle_;
    HV_CHECK(hv_vcpu_set_reg(v, HV_REG_PC,   pc),          "set PC");
    HV_CHECK(hv_vcpu_set_reg(v, HV_REG_X0,   x0),          "set X0");
    HV_CHECK(hv_vcpu_set_reg(v, HV_REG_X1,   0),           "set X1");
    HV_CHECK(hv_vcpu_set_reg(v, HV_REG_X2,   0),           "set X2");
    HV_CHECK(hv_vcpu_set_reg(v, HV_REG_X3,   0),           "set X3");
    HV_CHECK(hv_vcpu_set_reg(v, HV_REG_CPSR, kPstateEl1h), "set CPSR");

    // Minimal EL1 system registers required for Linux entry.
    HV_CHECK(hv_vcpu_set_sys_reg(v, HV_SYS_REG_SCTLR_EL1,
                                 0x00C50078ULL), "set SCTLR_EL1");
    HV_CHECK(hv_vcpu_set_sys_reg(v, HV_SYS_REG_CPACR_EL1,
                                 0x00300000ULL), "set CPACR_EL1"); // FPEN = 0b11
    HV_CHECK(hv_vcpu_set_sys_reg(v, HV_SYS_REG_MPIDR_EL1,
                                 0x80000000ULL | index_), "set MPIDR_EL1");
    return true;
}

void VCpu::thread_func() {
    const hv_return_t cr = hv_vcpu_create(&handle_, &exit_, nullptr);
    if (cr != HV_SUCCESS) {
        logger::log("[vcpu%u] hv_vcpu_create failed: 0x%x\n", index_, cr);
        ctx_.stop();
        return;
    }

    if (index_ == 0) {
        // Primary vCPU: boot at kernel entry with DTB GPA in X0.
        if (!setup_regs(ctx_.kernel_entry(), ctx_.dtb_gpa())) {
            ctx_.stop();
            hv_vcpu_destroy(handle_);
            return;
        }
    } else {
        // Secondary vCPUs: spin until PSCI CPU_ON sets boot_pending.
        while (!boot_pending.load(std::memory_order_acquire))
            sched_yield();
        const uint64_t pc = boot_pc.load(std::memory_order_relaxed);
        const uint64_t x0 = boot_x0.load(std::memory_order_relaxed);
        boot_pending.store(false, std::memory_order_release);
        if (!setup_regs(pc, x0)) {
            ctx_.stop();
            hv_vcpu_destroy(handle_);
            return;
        }
    }

    logger::log("[vcpu%u] running\n", index_);

    while (ctx_.running().load(std::memory_order_relaxed)) {
        const hv_return_t r = hv_vcpu_run(handle_);
        if (r != HV_SUCCESS) {
            logger::log("[vcpu%u] hv_vcpu_run failed: 0x%x\n", index_, r);
            break;
        }
        if (!ExitHandler::handle(*this)) break;
    }

    hv_vcpu_destroy(handle_);
    handle_ = 0;
    logger::log("[vcpu%u] thread exiting\n", index_);
}

} // namespace virtualization