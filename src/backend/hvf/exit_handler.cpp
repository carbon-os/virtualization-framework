#include "exit_handler.hpp"
#include "virtualization/backend/hvf/vcpu.hpp"
#include "virtualization/vm_context.hpp"
#include "virtualization/firmware/arm64/gic.hpp"
#include "virtualization/virtio/console.hpp"
#include "virtualization/virtio/block.hpp"
#include "virtualization/virtio/net.hpp"
#include "virtualization/virtio/gpu.hpp"
#include "virtualization/virtio/input.hpp"   // ADDED
#include "virtualization/logger.hpp"
#include <Hypervisor/Hypervisor.h>
#include <ctime>
#include <sched.h>
#include <unistd.h>

namespace virtualization { bool psci_dispatch(VCpu& vcpu); }

namespace virtualization {

namespace {

inline constexpr uint32_t kEcWfiWfe    = 0x01;
inline constexpr uint32_t kEcMsrMrs    = 0x18;
inline constexpr uint32_t kEcHvcA64   = 0x16;
inline constexpr uint32_t kEcSmcA64   = 0x17;
inline constexpr uint32_t kEcIabtLower = 0x20;
inline constexpr uint32_t kEcDabtLower = 0x24;

inline constexpr uint32_t kPl011Dr   = 0x000;
inline constexpr uint32_t kPl011Fr   = 0x018;
inline constexpr uint32_t kPl011Lcr  = 0x02C;
inline constexpr uint32_t kPl011Cr   = 0x030;
inline constexpr uint32_t kPl011Imsc = 0x038;
inline constexpr uint32_t kPl011Mis  = 0x040;
inline constexpr uint32_t kPl011Icr  = 0x044;

inline constexpr uint32_t kGicPidr2Offset = 0xFFE8u;
inline constexpr uint32_t kGicPidr2GICv3  = 0x3Bu;

inline hv_reg_t xreg_id(uint32_t n) noexcept {
    return static_cast<hv_reg_t>(HV_REG_X0 + n);
}

void advance_pc(VCpu& vcpu) noexcept {
    uint64_t pc = 0;
    hv_vcpu_get_reg(vcpu.handle(), HV_REG_PC, &pc);
    hv_vcpu_set_reg(vcpu.handle(), HV_REG_PC, pc + 4);
}

uint64_t get_xreg(VCpu& vcpu, uint32_t n) noexcept {
    if (n == 31) return 0;
    uint64_t v = 0;
    hv_vcpu_get_reg(vcpu.handle(), xreg_id(n), &v);
    return v;
}

void handle_msr_mrs(VCpu& vcpu) {
    const hv_vcpu_exit_t* ex  = vcpu.exit_info();
    const uint32_t        iss = static_cast<uint32_t>(ex->exception.syndrome) & 0x1FFFFFFU;
    const bool     is_read = (iss >> 4) & 1;
    const uint32_t rt      = iss & 0xF;
    if (is_read && rt < 31)
        hv_vcpu_set_reg(vcpu.handle(), xreg_id(rt), 0);
    advance_pc(vcpu);
}

bool handle_mmio(VCpu& vcpu) {
    VMContext&      ctx = vcpu.ctx_ref();
    hv_vcpu_exit_t* ex  = vcpu.exit_info();

    const uint64_t ipa = ex->exception.physical_address;
    const uint64_t esr = ex->exception.syndrome;
    const bool     wnr = (esr >> 6) & 1;
    const uint32_t srt = (esr >> 16) & 0x1F;
    const uint32_t sas = (esr >> 22) & 0x3;
    const uint32_t len = 1u << sas;

    // ── PL011 UART ────────────────────────────────────────────────────────────
    if (ipa >= kGpaUart0 && ipa < kGpaUart0 + kGpaUart0Size) {
        const uint32_t off   = static_cast<uint32_t>(ipa - kGpaUart0);
        Pl011State&    pl011 = ctx.pl011();
        if (wnr) {
            const uint64_t v = get_xreg(vcpu, srt);
            switch (off) {
                case kPl011Dr: { const char c = char(v & 0xFF); logger::write(&c, 1); break; }
                case kPl011Cr:   pl011.cr   = uint32_t(v); break;
                case kPl011Lcr:  pl011.lcr  = uint32_t(v); break;
                case kPl011Imsc: pl011.imsc = uint32_t(v); break;
                default: break;
            }
        } else {
            uint32_t v = 0;
            switch (off) {
                case kPl011Dr:   v = 0;        break;
                case kPl011Fr:   v = 0x90;     break;
                case kPl011Cr:   v = pl011.cr;   break;
                case kPl011Lcr:  v = pl011.lcr;  break;
                case kPl011Imsc: v = pl011.imsc; break;
                case kPl011Mis:  v = 0;        break;
                case 0xFE0: v = 0x11; break;
                case 0xFE4: v = 0x10; break;
                case 0xFE8: v = 0x14; break;
                case 0xFEC: v = 0x00; break;
                case 0xFF0: v = 0x0D; break;
                case 0xFF4: v = 0xF0; break;
                case 0xFF8: v = 0x05; break;
                case 0xFFC: v = 0xB1; break;
                default:    v = 0;    break;
            }
            hv_vcpu_set_reg(vcpu.handle(), xreg_id(srt), v);
        }
        advance_pc(vcpu);
        return true;
    }

    // ── PL031 RTC ─────────────────────────────────────────────────────────────
    if (ipa >= kGpaRtcBase && ipa < kGpaRtcBase + kGpaRtcSize) {
        const uint32_t off = static_cast<uint32_t>(ipa - kGpaRtcBase);
        if (!wnr) {
            uint32_t val = 0;
            switch (off) {
                case 0x000: val = uint32_t(::time(nullptr)); break;
                case 0xFE0: val = 0x31; break;
                case 0xFE4: val = 0x10; break;
                case 0xFE8: val = 0x04; break;
                case 0xFEC: val = 0x00; break;
                case 0xFF0: val = 0x0D; break;
                case 0xFF4: val = 0xF0; break;
                case 0xFF8: val = 0x05; break;
                case 0xFFC: val = 0xB1; break;
                default:    val = 0;    break;
            }
            hv_vcpu_set_reg(vcpu.handle(), xreg_id(srt), val);
        }
        advance_pc(vcpu);
        return true;
    }

    // ── virtio-mmio console ───────────────────────────────────────────────────
    if (ipa >= kGpaVirtioConsole && ipa < kGpaVirtioConsole + kGpaVirtioStride) {
        if (ctx.console()) {
            const uint32_t off = uint32_t(ipa - kGpaVirtioConsole);
            if (wnr) ctx.console()->write(off, uint32_t(get_xreg(vcpu, srt)), len);
            else     hv_vcpu_set_reg(vcpu.handle(), xreg_id(srt), ctx.console()->read(off, len));
        } else {
            if (!wnr) hv_vcpu_set_reg(vcpu.handle(), xreg_id(srt), 0);
        }
        advance_pc(vcpu);
        return true;
    }

    // ── virtio-mmio block ─────────────────────────────────────────────────────
    if (ipa >= kGpaVirtioBlock && ipa < kGpaVirtioBlock + kGpaVirtioStride) {
        if (ctx.block()) {
            const uint32_t off = uint32_t(ipa - kGpaVirtioBlock);
            if (wnr) ctx.block()->write(off, uint32_t(get_xreg(vcpu, srt)), len);
            else     hv_vcpu_set_reg(vcpu.handle(), xreg_id(srt), ctx.block()->read(off, len));
        } else {
            if (!wnr) hv_vcpu_set_reg(vcpu.handle(), xreg_id(srt), 0);
        }
        advance_pc(vcpu);
        return true;
    }

    // ── virtio-mmio net ───────────────────────────────────────────────────────
    if (ipa >= kGpaVirtioNet && ipa < kGpaVirtioNet + kGpaVirtioStride) {
        if (ctx.net()) {
            const uint32_t off = uint32_t(ipa - kGpaVirtioNet);
            if (wnr) ctx.net()->write(off, uint32_t(get_xreg(vcpu, srt)), len);
            else     hv_vcpu_set_reg(vcpu.handle(), xreg_id(srt), ctx.net()->read(off, len));
        } else {
            if (!wnr) hv_vcpu_set_reg(vcpu.handle(), xreg_id(srt), 0);
        }
        advance_pc(vcpu);
        return true;
    }

    // ── virtio-mmio gpu ───────────────────────────────────────────────────────
    if (ipa >= kGpaVirtioGpu && ipa < kGpaVirtioGpu + kGpaVirtioStride) {
        if (ctx.gpu()) {
            const uint32_t off = uint32_t(ipa - kGpaVirtioGpu);
            if (wnr) ctx.gpu()->write(off, uint32_t(get_xreg(vcpu, srt)), len);
            else     hv_vcpu_set_reg(vcpu.handle(), xreg_id(srt), ctx.gpu()->read(off, len));
        } else {
            if (!wnr) hv_vcpu_set_reg(vcpu.handle(), xreg_id(srt), 0);
        }
        advance_pc(vcpu);
        return true;
    }

    // ── virtio-mmio keyboard ──────────────────────────────────────────────────
    if (ipa >= kGpaVirtioKeyboard && ipa < kGpaVirtioKeyboard + kGpaVirtioStride) {
        if (ctx.keyboard()) {
            const uint32_t off = uint32_t(ipa - kGpaVirtioKeyboard);
            if (wnr) ctx.keyboard()->write(off, uint32_t(get_xreg(vcpu, srt)), len);
            else     hv_vcpu_set_reg(vcpu.handle(), xreg_id(srt), ctx.keyboard()->read(off, len));
        } else {
            if (!wnr) hv_vcpu_set_reg(vcpu.handle(), xreg_id(srt), 0);
        }
        advance_pc(vcpu);
        return true;
    }

    // ── virtio-mmio tablet ────────────────────────────────────────────────────
    if (ipa >= kGpaVirtioTablet && ipa < kGpaVirtioTablet + kGpaVirtioStride) {
        if (ctx.tablet()) {
            const uint32_t off = uint32_t(ipa - kGpaVirtioTablet);
            if (wnr) ctx.tablet()->write(off, uint32_t(get_xreg(vcpu, srt)), len);
            else     hv_vcpu_set_reg(vcpu.handle(), xreg_id(srt), ctx.tablet()->read(off, len));
        } else {
            if (!wnr) hv_vcpu_set_reg(vcpu.handle(), xreg_id(srt), 0);
        }
        advance_pc(vcpu);
        return true;
    }

    // ── GIC distributor / redistributor ──────────────────────────────────────
    const uint64_t kGpaGicRedistSize = kVcpuMax * kGpaGicRedistStride;
    if ((ipa >= kGpaGicDist   && ipa < kGpaGicDist   + kGpaGicDistSize) ||
        (ipa >= kGpaGicRedist && ipa < kGpaGicRedist + kGpaGicRedistSize)) {
        if (!wnr) {
            uint32_t val = 0;
            if (ipa >= kGpaGicRedist && ipa < kGpaGicRedist + kGpaGicRedistSize) {
                const uint32_t frame_off = uint32_t(ipa - kGpaGicRedist) & 0xFFFFu;
                if (frame_off == kGicPidr2Offset)
                    val = kGicPidr2GICv3;
                else
                    logger::log("[exit] unhandled GICR read IPA 0x%llx\n",
                                static_cast<unsigned long long>(ipa));
            } else {
                logger::log("[exit] unexpected GICD read IPA 0x%llx\n",
                            static_cast<unsigned long long>(ipa));
            }
            hv_vcpu_set_reg(vcpu.handle(), xreg_id(srt), val);
        }
        advance_pc(vcpu);
        return true;
    }

    logger::log("[exit] unhandled MMIO %s IPA 0x%llx len=%u\n",
                wnr ? "W" : "R",
                static_cast<unsigned long long>(ipa), len);
    advance_pc(vcpu);
    return true;
}

} // anonymous namespace

bool ExitHandler::handle(VCpu& vcpu) {
    const hv_vcpu_exit_t* ex = vcpu.exit_info();

    switch (ex->reason) {
        case HV_EXIT_REASON_CANCELED:
            return true;
        case HV_EXIT_REASON_VTIMER_ACTIVATED:
            hv_vcpu_set_vtimer_mask(vcpu.handle(), false);
            return true;
        case HV_EXIT_REASON_EXCEPTION: {
            const uint64_t esr = ex->exception.syndrome;
            const uint32_t ec  = (esr >> 26) & 0x3F;
            switch (ec) {
                case kEcWfiWfe:
                    sched_yield();
                    advance_pc(vcpu);
                    return true;
                case kEcMsrMrs:
                    handle_msr_mrs(vcpu);
                    return true;
                case kEcHvcA64:
                case kEcSmcA64:
                    return psci_dispatch(vcpu);
                case kEcIabtLower:
                case kEcDabtLower:
                    return handle_mmio(vcpu);
                default:
                    logger::log("[exit] vcpu%u unhandled EC=0x%02x ESR=0x%016llx\n",
                                vcpu.index(), ec,
                                static_cast<unsigned long long>(esr));
                    advance_pc(vcpu);
                    return true;
            }
        }
        default:
            logger::log("[exit] vcpu%u unknown exit reason 0x%x\n",
                        vcpu.index(), ex->reason);
            return false;
    }
}

} // namespace virtualization