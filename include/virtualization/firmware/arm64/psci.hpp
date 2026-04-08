#pragma once
#include <cstdint>

namespace virtualization {

// ── PSCI function IDs (SMCCC 32-bit / AArch64 HVC/SMC) ───────────
inline constexpr uint32_t kPsciVersion       = 0x84000000U;
inline constexpr uint32_t kPsciCpuSuspend    = 0xC4000001U;
inline constexpr uint32_t kPsciCpuOff        = 0x84000002U;
inline constexpr uint32_t kPsciCpuOn         = 0xC4000003U;
inline constexpr uint32_t kPsciMigrateInfo   = 0x84000006U;
inline constexpr uint32_t kPsciSystemOff     = 0x84000008U;
inline constexpr uint32_t kPsciSystemReset   = 0x84000009U;

// ── PSCI return codes (sign-extended 64-bit into X0) ─────────────
inline constexpr uint64_t kPsciRetSuccess       =  0ULL;
inline constexpr uint64_t kPsciRetNotSupported  = static_cast<uint64_t>(-1LL);
inline constexpr uint64_t kPsciRetInvalidParams = static_cast<uint64_t>(-2LL);

} // namespace virtualization