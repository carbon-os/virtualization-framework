#include "virtualization/loader.hpp"
#include "virtualization/vm_context.hpp"
#include "virtualization/logger.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

namespace virtualization {

namespace {

struct Arm64ImageHdr {
    uint32_t code0, code1;
    uint64_t text_offset;
    uint64_t image_size;
    uint64_t flags;
    uint64_t res[3];
    uint32_t magic;
    uint32_t res5;
} __attribute__((packed));

inline constexpr uint32_t kArm64Magic = 0x644D5241U; // "ARM\x64"

bool load_file(std::string_view path, void* dst,
               std::size_t max_size, std::size_t& out_size) {
    std::string p{path};
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) {
        logger::log("[loader] cannot open %s: %s\n", p.c_str(), strerror(errno));
        return false;
    }

    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::rewind(f);

    if (sz <= 0 || static_cast<std::size_t>(sz) > max_size) {
        logger::log("[loader] %s: size %ld out of range (max=%zu)\n",
                    p.c_str(), sz, max_size);
        std::fclose(f);
        return false;
    }

    const auto usz = static_cast<std::size_t>(sz);
    if (std::fread(dst, 1, usz, f) != usz) {
        logger::log("[loader] fread failed for %s: %s\n",
                    p.c_str(), strerror(errno));
        std::fclose(f);
        return false;
    }

    std::fclose(f);
    out_size = usz;
    return true;
}

} // namespace

bool Loader::load_kernel(VMContext& ctx, std::string_view path) {
    void*       dst = ctx.gpa_to_hva(kGpaKernelLoad);
    std::size_t sz  = 0;

    if (!dst) {
        logger::log("[loader] kernel load GPA 0x%llx not mapped\n",
                    static_cast<unsigned long long>(kGpaKernelLoad));
        return false;
    }

    if (!load_file(path, dst, ctx.ram_size() / 2, sz)) return false;

    const auto* hdr = static_cast<const Arm64ImageHdr*>(dst);
    if (hdr->magic != kArm64Magic) {
        logger::log("[loader] bad arm64 magic: expected 0x%08x got 0x%08x\n",
                    kArm64Magic, hdr->magic);
        return false;
    }

    ctx.set_kernel_entry(kGpaKernelLoad + hdr->text_offset);
    logger::log("[loader] kernel: %zu bytes  entry GPA 0x%llx\n",
                sz, static_cast<unsigned long long>(ctx.kernel_entry()));
    return true;
}

bool Loader::load_initrd(VMContext& ctx, std::string_view path) {
    void*       dst = ctx.gpa_to_hva(kGpaInitrdLoad);
    std::size_t sz  = 0;

    if (!dst) {
        logger::log("[loader] initrd load GPA 0x%llx not mapped\n",
                    static_cast<unsigned long long>(kGpaInitrdLoad));
        return false;
    }

    if (!load_file(path, dst, ctx.ram_size() / 4, sz)) return false;

    ctx.set_initrd(kGpaInitrdLoad, static_cast<uint64_t>(sz));
    logger::log("[loader] initrd: %zu bytes at GPA 0x%llx\n",
                sz, static_cast<unsigned long long>(kGpaInitrdLoad));
    return true;
}

} // namespace virtualization