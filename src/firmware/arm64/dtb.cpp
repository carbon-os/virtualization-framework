#include "virtualization/firmware/arm64/dtb.hpp"
#include "virtualization/vm_context.hpp"
#include "virtualization/firmware/arm64/gic.hpp"
#include "virtualization/virtio/gpu.hpp"
#include "virtualization/virtio/input.hpp"
#include "virtualization/logger.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

namespace virtualization {

namespace {

static constexpr uint32_t kFdtMagic     = 0xd00dfeedU;
static constexpr uint32_t kFdtVersion   = 17;
static constexpr uint32_t kFdtCompVer   = 16;
static constexpr uint32_t kFdtHdrSize   = 40;
static constexpr uint32_t kFdtBeginNode = 0x00000001U;
static constexpr uint32_t kFdtEndNode   = 0x00000002U;
static constexpr uint32_t kFdtProp      = 0x00000003U;
static constexpr uint32_t kFdtEnd       = 0x00000009U;

static constexpr uint32_t kPhGic   = 1;
static constexpr uint32_t kPhClock = 2;

static void push_be32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >>  8)); v.push_back(uint8_t(x));
}
static void push_be64(std::vector<uint8_t>& v, uint64_t x) {
    push_be32(v, uint32_t(x >> 32)); push_be32(v, uint32_t(x));
}
static void pad4(std::vector<uint8_t>& v) {
    while (v.size() & 3u) v.push_back(0);
}

class FdtBuilder {
public:
    void begin_node(const char* name) {
        push_be32(st_, kFdtBeginNode);
        for (const char* p = name; *p; ++p) st_.push_back(uint8_t(*p));
        st_.push_back(0); pad4(st_);
    }
    void end_node() { push_be32(st_, kFdtEndNode); }

    void prop(const char* name, const void* data, uint32_t len) {
        push_be32(st_, kFdtProp);
        push_be32(st_, len);
        push_be32(st_, str_off(name));
        if (data && len) {
            const uint8_t* d = static_cast<const uint8_t*>(data);
            for (uint32_t i = 0; i < len; ++i) st_.push_back(d[i]);
        }
        pad4(st_);
    }
    void prop_u32(const char* name, uint32_t v) {
        const uint8_t b[4] = { uint8_t(v>>24), uint8_t(v>>16), uint8_t(v>>8), uint8_t(v) };
        prop(name, b, 4);
    }
    void prop_str(const char* name, const char* s) {
        prop(name, s, uint32_t(std::strlen(s)) + 1);
    }
    void prop_strlist(const char* name, std::initializer_list<const char*> strs) {
        std::vector<uint8_t> buf;
        for (const char* s : strs) { while (*s) buf.push_back(uint8_t(*s++)); buf.push_back(0); }
        prop(name, buf.data(), uint32_t(buf.size()));
    }
    void prop_cells(const char* name, std::initializer_list<uint32_t> cells) {
        std::vector<uint8_t> buf; buf.reserve(cells.size() * 4);
        for (uint32_t c : cells) push_be32(buf, c);
        prop(name, buf.data(), uint32_t(buf.size()));
    }
    void prop_empty(const char* name) { prop(name, nullptr, 0); }

    void reserve(uint64_t addr, uint64_t size) {
        push_be64(rsv_, addr); push_be64(rsv_, size);
    }

    std::vector<uint8_t> build() {
        push_be32(st_, kFdtEnd);
        push_be64(rsv_, 0); push_be64(rsv_, 0);

        const uint32_t off_rsv = kFdtHdrSize;
        const uint32_t off_st  = off_rsv + uint32_t(rsv_.size());
        const uint32_t off_str = off_st  + uint32_t(st_.size());
        const uint32_t total   = off_str + uint32_t(str_.size());

        std::vector<uint8_t> blob; blob.reserve(total);
        push_be32(blob, kFdtMagic);  push_be32(blob, total);
        push_be32(blob, off_st);     push_be32(blob, off_str);
        push_be32(blob, off_rsv);    push_be32(blob, kFdtVersion);
        push_be32(blob, kFdtCompVer);push_be32(blob, 0);
        push_be32(blob, uint32_t(str_.size()));
        push_be32(blob, uint32_t(st_.size()));
        blob.insert(blob.end(), rsv_.begin(), rsv_.end());
        blob.insert(blob.end(), st_.begin(),  st_.end());
        blob.insert(blob.end(), str_.begin(), str_.end());
        return blob;
    }

private:
    std::vector<uint8_t>                      st_, str_, rsv_;
    std::unordered_map<std::string, uint32_t> str_map_;

    uint32_t str_off(const char* name) {
        const std::string key(name);
        auto it = str_map_.find(key);
        if (it != str_map_.end()) return it->second;
        const uint32_t off = uint32_t(str_.size());
        for (char c : key) str_.push_back(uint8_t(c));
        str_.push_back(0);
        str_map_[key] = off;
        return off;
    }
};

} // anonymous namespace

bool DTB::generate(VMContext& ctx) {
    FdtBuilder b;

    b.reserve(kGpaDtbLoad, kGpaKernelLoad - kGpaDtbLoad);

    b.begin_node("");
    b.prop_u32    ("#address-cells",   2);
    b.prop_u32    ("#size-cells",      2);
    b.prop_u32    ("interrupt-parent", kPhGic);
    b.prop_strlist("compatible",       {"linux,dummy-virt"});
    b.prop_str    ("model",            "virtualization-vm");

    b.begin_node("chosen");
    b.prop_str("bootargs",    ctx.cmdline().c_str());
    b.prop_str("stdout-path", "/pl011@9000000");
    if (ctx.initrd_size() > 0) {
        const uint64_t start = ctx.initrd_gpa();
        const uint64_t end   = start + ctx.initrd_size();
        b.prop_cells("linux,initrd-start", {uint32_t(start>>32), uint32_t(start)});
        b.prop_cells("linux,initrd-end",   {uint32_t(end  >>32), uint32_t(end)});
    }
    b.end_node();

    {
        char name[32];
        std::snprintf(name, sizeof(name), "memory@%llx",
                      static_cast<unsigned long long>(ctx.ram_gpa()));
        b.begin_node(name);
        b.prop_str  ("device_type", "memory");
        b.prop_cells("reg", {
            uint32_t(ctx.ram_gpa()  >> 32), uint32_t(ctx.ram_gpa()),
            uint32_t(ctx.ram_size() >> 32), uint32_t(ctx.ram_size()),
        });
        b.end_node();
    }

    b.begin_node("psci");
    b.prop_strlist("compatible", {"arm,psci-1.0", "arm,psci-0.2", "arm,psci"});
    b.prop_str    ("method",     "hvc");
    b.end_node();

    b.begin_node("cpus");
    b.prop_u32("#address-cells", 1);
    b.prop_u32("#size-cells",    0);
    for (uint32_t i = 0; i < ctx.vcpu_count(); ++i) {
        char name[16];
        std::snprintf(name, sizeof(name), "cpu@%u", i);
        b.begin_node(name);
        b.prop_strlist("compatible",    {"arm,cortex-a72", "arm,armv8"});
        b.prop_str    ("device_type",   "cpu");
        b.prop_str    ("enable-method", "psci");
        b.prop_u32    ("reg",           i);
        b.end_node();
    }
    b.end_node();

    b.begin_node("timer");
    b.prop_strlist("compatible", {"arm,armv8-timer"});
    b.prop_cells  ("interrupts", { 1,13,0xf04, 1,14,0xf04, 1,11,0xf04, 1,10,0xf04 });
    b.prop_empty("always-on");
    b.end_node();

    {
        const uint64_t gicr_size = uint64_t(kVcpuMax) * kGpaGicRedistStride;
        b.begin_node("intc@8000000");
        b.prop_strlist("compatible",          {"arm,gic-v3"});
        b.prop_u32    ("#interrupt-cells",    3);
        b.prop_empty  ("interrupt-controller");
        b.prop_cells  ("reg", {
            0, uint32_t(kGpaGicDist),   0, uint32_t(kGpaGicDistSize),
            0, uint32_t(kGpaGicRedist), 0, uint32_t(gicr_size),
        });
        b.prop_cells("interrupts",    {1, 9, 0xf04});
        b.prop_u32  ("phandle",       kPhGic);
        b.prop_u32  ("linux,phandle", kPhGic);
        b.end_node();
    }

    b.begin_node("apb-clk");
    b.prop_strlist("compatible",         {"fixed-clock"});
    b.prop_u32    ("#clock-cells",       0);
    b.prop_u32    ("clock-frequency",    24000000u);
    b.prop_str    ("clock-output-names", "clk24mhz");
    b.prop_u32    ("phandle",            kPhClock);
    b.prop_u32    ("linux,phandle",      kPhClock);
    b.end_node();

    b.begin_node("pl011@9000000");
    b.prop_strlist("compatible",  {"arm,pl011", "arm,primecell"});
    b.prop_cells  ("reg",         {0, uint32_t(kGpaUart0),   0, uint32_t(kGpaUart0Size)});
    b.prop_cells  ("interrupts",  {0, 1, 4});
    b.prop_cells  ("clocks",      {kPhClock, kPhClock});
    b.prop_strlist("clock-names", {"uartclk", "apb_pclk"});
    b.end_node();

    b.begin_node("pl031@9010000");
    b.prop_strlist("compatible",  {"arm,pl031", "arm,primecell"});
    b.prop_cells  ("reg",         {0, uint32_t(kGpaRtcBase), 0, uint32_t(kGpaRtcSize)});
    b.prop_cells  ("interrupts",  {0, 2, 4});
    b.prop_cells  ("clocks",      {kPhClock});
    b.prop_strlist("clock-names", {"apb_pclk"});
    b.end_node();

    struct VirtioEntry { const char* name; uint64_t gpa; uint32_t spi; };
    static constexpr VirtioEntry kVirtioDevs[] = {
        {"virtio_mmio@a000000", kGpaVirtioConsole, kSpiConsole},
        {"virtio_mmio@a000200", kGpaVirtioBlock,   kSpiBlock  },
        {"virtio_mmio@a000400", kGpaVirtioNet,     kSpiNet    },
    };
    for (const auto& dev : kVirtioDevs) {
        b.begin_node(dev.name);
        b.prop_strlist("compatible", {"virtio,mmio"});
        b.prop_cells  ("reg",        {0, uint32_t(dev.gpa), 0, uint32_t(kGpaVirtioStride)});
        b.prop_cells  ("interrupts", {0, dev.spi, 1});
        b.prop_empty  ("dma-coherent");
        b.end_node();
    }

    if (ctx.gpu()) {
        b.begin_node("virtio_mmio@a000600");
        b.prop_strlist("compatible", {"virtio,mmio"});
        b.prop_cells  ("reg",        {0, uint32_t(kGpaVirtioGpu), 0, uint32_t(kGpaVirtioStride)});
        b.prop_cells  ("interrupts", {0, kSpiGpu, 1});
        b.prop_empty  ("dma-coherent");
        b.end_node();
    }

    // ── virtio-input ─────────────────────────────────────────────────────────
    // FIXED: use explicit named SPI constants from gic.hpp.
    // The old code used kSpiConsole + slot_index which gave
    // kSpiConsole+4 = 20 = kSpiGpu — a collision that corrupted GIC
    // interrupt routing and prevented SPI 17 (block) completions.
    if (ctx.keyboard()) {
        b.begin_node("virtio_mmio@a000800");
        b.prop_strlist("compatible", {"virtio,mmio"});
        b.prop_cells  ("reg",        {0, uint32_t(kGpaVirtioKeyboard), 0,
                                      uint32_t(kGpaVirtioStride)});
        b.prop_cells  ("interrupts", {0, kSpiKeyboard, 1});
        b.prop_empty  ("dma-coherent");
        b.end_node();
    }

    if (ctx.tablet()) {
        b.begin_node("virtio_mmio@a000a00");
        b.prop_strlist("compatible", {"virtio,mmio"});
        b.prop_cells  ("reg",        {0, uint32_t(kGpaVirtioTablet), 0,
                                      uint32_t(kGpaVirtioStride)});
        b.prop_cells  ("interrupts", {0, kSpiTablet, 1});
        b.prop_empty  ("dma-coherent");
        b.end_node();
    }

    b.end_node(); // root

    const std::vector<uint8_t> blob = b.build();
    void* const dst = ctx.gpa_to_hva(kGpaDtbLoad);
    if (!dst) return false;

    const std::size_t room = kGpaKernelLoad - kGpaDtbLoad;
    if (blob.size() > room) return false;

    std::memcpy(dst, blob.data(), blob.size());
    ctx.set_dtb_gpa(kGpaDtbLoad);

    logger::log("[dtb] %zu bytes at GPA 0x%llx\n",
                blob.size(), static_cast<unsigned long long>(kGpaDtbLoad));
    return true;
}

} // namespace virtualization