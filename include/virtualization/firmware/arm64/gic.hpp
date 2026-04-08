#pragma once
#include <cstdint>
#include <Hypervisor/Hypervisor.h>

namespace virtualization {

// ── GICv3 guest-physical base addresses ──────────────────────────
inline constexpr uint64_t kGpaGicDist         = 0x08000000ULL;
inline constexpr uint64_t kGpaGicDistSize     = 0x00010000ULL;
inline constexpr uint64_t kGpaGicRedist       = 0x080A0000ULL;
inline constexpr uint64_t kGpaGicRedistStride = 0x00020000ULL;

// ── Interrupt numbering ───────────────────────────────────────────
inline constexpr uint32_t kGicSpiBase  = 32;

// Per-device SPI assignments (must match DTB node "interrupts" cells exactly)
inline constexpr uint32_t kSpiConsole  = 16;
inline constexpr uint32_t kSpiBlock    = 17;
inline constexpr uint32_t kSpiNet      = 19;
inline constexpr uint32_t kSpiGpu      = 20;
inline constexpr uint32_t kSpiKeyboard = 21; // was colliding with kSpiGpu via formula
inline constexpr uint32_t kSpiTablet   = 22;

// ── Simple-framebuffer reservation ───────────────────────────────
inline constexpr uint64_t kGpaFramebufferSize = 16ULL * 1024 * 1024;

/// Pulse one SPI on the GICv3 emulated by Hypervisor.framework.
void raise_spi(uint32_t spi_number) noexcept;

} // namespace virtualization