#pragma once
// Internal header — not part of the public API surface.
// Equivalent to VMM in the reference implementation; holds all guest state
// and acts as the dependency hub for all backend / firmware / virtio classes.

#include "virtualization/logger.hpp"
#include "virtualization/firmware/arm64/gic.hpp"
#include "virtualization/firmware/arm64/psci.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <Hypervisor/Hypervisor.h>

namespace virtualization {

// ── Guest physical address map ────────────────────────────────────
inline constexpr uint64_t kGpaRamBase       = 0x40000000ULL;
inline constexpr uint64_t kGpaDtbLoad       = 0x40000000ULL;
inline constexpr uint64_t kGpaKernelLoad    = 0x40400000ULL;
inline constexpr uint64_t kGpaInitrdLoad    = 0x48000000ULL;
inline constexpr uint64_t kGpaUart0         = 0x09000000ULL;
inline constexpr uint64_t kGpaUart0Size     = 0x00001000ULL;
inline constexpr uint64_t kGpaRtcBase       = 0x09010000ULL;
inline constexpr uint64_t kGpaRtcSize       = 0x00001000ULL;

inline constexpr uint64_t kGpaVirtioBase    = 0x0A000000ULL;
inline constexpr uint64_t kGpaVirtioStride  = 0x00000200ULL;

inline constexpr uint64_t kGpaVirtioConsole = kGpaVirtioBase + 0 * kGpaVirtioStride;
inline constexpr uint64_t kGpaVirtioBlock   = kGpaVirtioBase + 1 * kGpaVirtioStride;
inline constexpr uint64_t kGpaVirtioNet     = kGpaVirtioBase + 2 * kGpaVirtioStride;
inline constexpr uint64_t kGpaVirtioGpu     = kGpaVirtioBase + 3 * kGpaVirtioStride;
inline constexpr uint64_t kGpaVirtioKeyboard= kGpaVirtioBase + 4 * kGpaVirtioStride;
inline constexpr uint64_t kGpaVirtioTablet  = kGpaVirtioBase + 5 * kGpaVirtioStride;

// ── VM limits / AArch64 initial processor state ───────────────────
inline constexpr uint32_t kVcpuMax    = 8;
inline constexpr uint64_t kPstateEl1h = 0x3C5ULL;

// ── Forward declarations ──────────────────────────────────────────
class VCpu;
namespace virtio { class Console; class Block; class Net; class Gpu; class Input; }

// ── PL011 soft state ──────────────────────────────────────────────
struct Pl011State {
    uint32_t cr   {0x300};
    uint32_t lcr  {0};
    uint32_t imsc {0};
};

// ── Internal network device config ────────────────────────────────
struct PortForwardEntry { uint16_t host_port, guest_port; };

struct NetDeviceConfig {
    bool    enabled {false};
    uint8_t mac[6]  {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    std::vector<PortForwardEntry> port_forwards;
};

// ── Frame callback type ───────────────────────────────────────────
using GpuFrameCallback = std::function<void(const uint8_t* pixels,
                                             uint32_t       width,
                                             uint32_t       height,
                                             uint32_t       stride)>;

// ─────────────────────────────────────────────────────────────────
// VMContext — central guest state
// ─────────────────────────────────────────────────────────────────
class VMContext {
public:
    explicit VMContext(uint32_t vcpu_count, uint64_t ram_size) noexcept;
    ~VMContext();

    VMContext(const VMContext&)            = delete;
    VMContext& operator=(const VMContext&) = delete;
    VMContext(VMContext&&)                 = delete;
    VMContext& operator=(VMContext&&)      = delete;

    // ── Subsystem construction ────────────────────────────────────
    [[nodiscard]] bool init_vcpus();
    [[nodiscard]] bool create_console();
    [[nodiscard]] bool create_block(std::string_view image_path);
    [[nodiscard]] bool create_net(const NetDeviceConfig& cfg);
    
    [[nodiscard]] bool create_gpu(uint32_t         width,
                                  uint32_t         height,
                                  GpuFrameCallback cb);

    [[nodiscard]] bool create_keyboard();
    [[nodiscard]] bool create_tablet(uint32_t width, uint32_t height);

    // ── Lifecycle ─────────────────────────────────────────────────
    void run_all();
    void stop() noexcept { running_.store(false, std::memory_order_relaxed); }

    // ── GPA ↔ HVA translation ─────────────────────────────────────
    [[nodiscard]] void* gpa_to_hva(uint64_t gpa) const noexcept;

    // ── RAM registration (called by Memory::init) ─────────────────
    void set_ram(void* hva, uint64_t gpa, uint64_t size,
                 std::string shm_path = {}) noexcept;

    // ── Immutable accessors ───────────────────────────────────────
    [[nodiscard]] void* ram_hva()      const noexcept { return ram_hva_;      }
    [[nodiscard]] uint64_t           ram_gpa()      const noexcept { return ram_gpa_;      }
    [[nodiscard]] uint64_t           ram_size()     const noexcept { return ram_size_;     }
    [[nodiscard]] const std::string& ram_shm_path() const noexcept { return ram_shm_path_; }
    [[nodiscard]] uint64_t           kernel_entry() const noexcept { return kernel_entry_; }
    [[nodiscard]] uint64_t           initrd_gpa()   const noexcept { return initrd_gpa_;   }
    [[nodiscard]] uint64_t           initrd_size()  const noexcept { return initrd_size_;  }
    [[nodiscard]] uint64_t           dtb_gpa()      const noexcept { return dtb_gpa_;      }
    [[nodiscard]] uint32_t           vcpu_count()   const noexcept { return vcpu_count_;   }
    [[nodiscard]] const std::string& cmdline()      const noexcept { return cmdline_;      }

    // ── Mutable accessors ─────────────────────────────────────────
    [[nodiscard]] std::atomic<bool>& running() noexcept { return running_; }
    [[nodiscard]] Pl011State&        pl011()   noexcept { return pl011_;   }

    [[nodiscard]] virtio::Console* console()  const noexcept { return console_.get(); }
    [[nodiscard]] virtio::Block* block()    const noexcept { return block_.get();   }
    [[nodiscard]] virtio::Net* net()      const noexcept { return net_.get();     }
    [[nodiscard]] virtio::Gpu* gpu()      const noexcept { return gpu_.get();     }
    [[nodiscard]] virtio::Input* keyboard() const noexcept { return keyboard_.get(); }
    [[nodiscard]] virtio::Input* tablet()   const noexcept { return tablet_.get(); }

    /// @throws std::out_of_range if i >= vcpu_count()
    [[nodiscard]] VCpu& vcpu(uint32_t i) { return *vcpus_.at(i); }

    // ── Mutators ──────────────────────────────────────────────────
    void set_kernel_entry(uint64_t entry)        noexcept { kernel_entry_ = entry; }
    void set_initrd(uint64_t gpa, uint64_t size) noexcept { initrd_gpa_ = gpa; initrd_size_ = size; }
    void set_dtb_gpa(uint64_t gpa)               noexcept { dtb_gpa_ = gpa; }
    void set_cmdline(std::string_view cl)        noexcept { cmdline_ = cl; }

private:
    void* ram_hva_      {nullptr};
    uint64_t          ram_gpa_      {kGpaRamBase};
    uint64_t          ram_size_;
    std::string       ram_shm_path_;
    uint64_t          kernel_entry_ {0};
    uint64_t          initrd_gpa_   {0};
    uint64_t          initrd_size_  {0};
    uint64_t          dtb_gpa_      {0};
    uint32_t          vcpu_count_;
    std::atomic<bool> running_      {false};
    std::string       cmdline_;

    Pl011State                           pl011_;
    std::vector<std::unique_ptr<VCpu>>   vcpus_;
    std::unique_ptr<virtio::Console>     console_;
    std::unique_ptr<virtio::Block>       block_;
    std::unique_ptr<virtio::Net>         net_;
    std::unique_ptr<virtio::Gpu>         gpu_;
    std::unique_ptr<virtio::Input>       keyboard_;
    std::unique_ptr<virtio::Input>       tablet_;
};

// ── Hypervisor.framework result helper ───────────────────────────
[[nodiscard]] inline bool hv_check(hv_return_t r, const char* msg) noexcept {
    if (r != HV_SUCCESS) {
        logger::log("[hv] %s failed: 0x%x\n", msg, static_cast<unsigned>(r));
        return false;
    }
    return true;
}

#define HV_CHECK(expr, msg) \
    do { if (!::virtualization::hv_check((expr), (msg))) return false; } while (0)

} // namespace virtualization