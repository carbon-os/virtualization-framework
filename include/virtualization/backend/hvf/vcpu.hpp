#pragma once
#include "virtualization/vm_context.hpp"
#include <atomic>
#include <thread>
#include <Hypervisor/Hypervisor.h>

namespace virtualization {

/// One hardware virtual CPU backed by Hypervisor.framework (Apple HVF).
/// Owns its OS thread; the thread is spawned by spawn() and joined by
/// join() / destructor.
class VCpu {
public:
    VCpu(VMContext& ctx, uint32_t index) noexcept;

    // join() is called from the destructor; safe to call multiple times.
    ~VCpu();

    VCpu(const VCpu&)            = delete;
    VCpu& operator=(const VCpu&) = delete;
    VCpu(VCpu&&)                 = delete;
    VCpu& operator=(VCpu&&)      = delete;

    /// Starts the vCPU thread.  Must be called exactly once.
    void spawn();

    /// Waits for the vCPU thread to exit.
    void join();

    // ── Accessors ────────────────────────────────────────────────
    [[nodiscard]] uint32_t        index()    const noexcept { return index_;  }
    [[nodiscard]] hv_vcpu_t       handle()   const noexcept { return handle_; }
    [[nodiscard]] hv_vcpu_exit_t* exit_info()const noexcept { return exit_;   }
    [[nodiscard]] VMContext&      ctx_ref()        noexcept { return ctx_;    }

    // ── PSCI CPU_ON handshake ─────────────────────────────────────
    // Requester writes boot_pc / boot_x0, then sets boot_pending.
    // Target thread spins on boot_pending, reads pc/x0, then clears it.
    std::atomic<uint64_t> boot_pc      {0};
    std::atomic<uint64_t> boot_x0      {0};
    std::atomic<bool>     boot_pending {false};

private:
    VMContext&       ctx_;
    uint32_t         index_;
    hv_vcpu_t        handle_ {0};
    hv_vcpu_exit_t*  exit_   {nullptr};
    std::thread      thread_;

    void               thread_func();
    [[nodiscard]] bool setup_regs(uint64_t pc, uint64_t x0);
};

} // namespace virtualization