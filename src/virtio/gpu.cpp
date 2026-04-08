#include "virtualization/virtio/gpu.hpp"
#include "virtualization/firmware/arm64/gic.hpp"
#include "virtualization/vm_context.hpp"
#include "virtualization/logger.hpp"
#include <algorithm>
#include <cstring>
#include <vector>

namespace virtualization::virtio {

// ── Virtio MMIO register offsets ─────────────────────────────────────────────
namespace Reg {
    inline constexpr uint32_t Magic         = 0x000;
    inline constexpr uint32_t Version       = 0x004;
    inline constexpr uint32_t DeviceId      = 0x008;
    inline constexpr uint32_t VendorId      = 0x00C;
    inline constexpr uint32_t DeviceFeat    = 0x010;
    inline constexpr uint32_t DeviceFeatSel = 0x014;
    inline constexpr uint32_t DriverFeat    = 0x020;
    inline constexpr uint32_t DriverFeatSel = 0x024;
    inline constexpr uint32_t QueueSel      = 0x030;
    inline constexpr uint32_t QueueNumMax   = 0x034;
    inline constexpr uint32_t QueueNum      = 0x038;
    inline constexpr uint32_t QueueReady    = 0x044;
    inline constexpr uint32_t QueueNotify   = 0x050;
    inline constexpr uint32_t IrqStatus     = 0x060;
    inline constexpr uint32_t IrqAck        = 0x064;
    inline constexpr uint32_t Status        = 0x070;
    inline constexpr uint32_t QueueDescLo   = 0x080;
    inline constexpr uint32_t QueueDescHi   = 0x084;
    inline constexpr uint32_t QueueAvailLo  = 0x090;
    inline constexpr uint32_t QueueAvailHi  = 0x094;
    inline constexpr uint32_t QueueUsedLo   = 0x0A0;
    inline constexpr uint32_t QueueUsedHi   = 0x0A4;

    // Virtio Shared Memory Regions
    inline constexpr uint32_t ShmSel        = 0x0AC;
    inline constexpr uint32_t ShmLenLo      = 0x0B0;
    inline constexpr uint32_t ShmLenHi      = 0x0B4;
    inline constexpr uint32_t ShmBaseLo     = 0x0B8;
    inline constexpr uint32_t ShmBaseHi     = 0x0BC;

    inline constexpr uint32_t ConfigGen     = 0x0FC;
    inline constexpr uint32_t ConfigBase    = 0x100;
}

// ── Feature bits ──────────────────────────────────────────────────────────────
inline constexpr uint64_t kVirtioF_Version1 = 1ULL << 32;

// ── Virtio status register bits ───────────────────────────────────────────────
inline constexpr uint32_t VIRTIO_STATUS_FEATURES_OK = 0x08u;

// ── Virtio-GPU command/response types ─────────────────────────────────────────
inline constexpr uint32_t VIRTIO_GPU_CMD_GET_DISPLAY_INFO        = 0x0100;
inline constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_CREATE_2D      = 0x0101;
inline constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_UNREF          = 0x0102;
inline constexpr uint32_t VIRTIO_GPU_CMD_SET_SCANOUT             = 0x0103;
inline constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_FLUSH          = 0x0104;
inline constexpr uint32_t VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D     = 0x0105;
inline constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING = 0x0106;
inline constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING = 0x0107;

inline constexpr uint32_t VIRTIO_GPU_RESP_OK_NODATA               = 0x1100;
inline constexpr uint32_t VIRTIO_GPU_RESP_OK_DISPLAY_INFO         = 0x1101;
inline constexpr uint32_t VIRTIO_GPU_RESP_ERR_UNSPEC              = 0x1200;
inline constexpr uint32_t VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID = 0x1203;

inline constexpr uint32_t VIRTIO_GPU_FLAG_FENCE = 1u;

inline constexpr uint16_t VRING_DESC_F_NEXT  = 1;
inline constexpr uint16_t VRING_DESC_F_WRITE = 2;

// ── Wire structs (packed, little-endian) ──────────────────────────────────────
struct GpuCtrlHdr {
    uint32_t type, flags;
    uint64_t fence_id;
    uint32_t ctx_id, padding;
} __attribute__((packed));

struct GpuRect   { uint32_t x, y, width, height; } __attribute__((packed));

struct GpuDisplayOne {
    GpuRect  r;
    uint32_t enabled, flags;
} __attribute__((packed));

struct GpuRespDisplayInfo {
    GpuCtrlHdr    hdr;
    GpuDisplayOne pmodes[16];
} __attribute__((packed));

struct GpuResourceCreate2d {
    GpuCtrlHdr hdr;
    uint32_t resource_id, format, width, height;
} __attribute__((packed));

struct GpuResourceUnref {
    GpuCtrlHdr hdr;
    uint32_t resource_id, padding;
} __attribute__((packed));

struct GpuSetScanout {
    GpuCtrlHdr hdr;
    GpuRect    r;
    uint32_t   scanout_id, resource_id;
} __attribute__((packed));

struct GpuResourceFlush {
    GpuCtrlHdr hdr;
    GpuRect    r;
    uint32_t   resource_id, padding;
} __attribute__((packed));

struct GpuTransferToHost2d {
    GpuCtrlHdr hdr;
    GpuRect    r;
    uint64_t   offset;
    uint32_t   resource_id, padding;
} __attribute__((packed));

struct GpuMemEntry {
    uint64_t addr;
    uint32_t length, padding;
} __attribute__((packed));

struct GpuResourceAttachBacking {
    GpuCtrlHdr hdr;
    uint32_t resource_id, nr_entries;
} __attribute__((packed));

struct GpuResourceDetachBacking {
    GpuCtrlHdr hdr;
    uint32_t resource_id, padding;
} __attribute__((packed));

// ── Constructor ───────────────────────────────────────────────────────────────

Gpu::Gpu(VMContext& ctx, uint64_t /*mmio_gpa*/,
         uint32_t width, uint32_t height,
         FrameCallback cb) noexcept
    : ctx_(ctx)
    , frame_cb_(std::move(cb))
    , width_(width)
    , height_(height)
    , device_features_(kVirtioF_Version1)
{
    logger::log("[gpu] virtio-gpu 2D initialized (%ux%u)\n", width_, height_);
}

void Gpu::notify_irq() { raise_spi(kSpiGpu); }

// ── Resource lookup ───────────────────────────────────────────────────────────

Gpu::Resource* Gpu::find_resource(uint32_t id) {
    auto it = resources_.find(id);
    return (it != resources_.end()) ? &it->second : nullptr;
}

// ── Backing-store reader ──────────────────────────────────────────────────────

size_t Gpu::read_backing(const Resource& res,
                          size_t          start_off,
                          uint8_t* dst,
                          size_t          len) const
{
    const size_t end_off  = start_off + len;
    size_t       copied   = 0;
    size_t       lin_base = 0;

    for (const auto& e : res.backing) {
        if (lin_base >= end_off) break;

        const size_t lin_end  = lin_base + e.len;
        const size_t ov_start = std::max(lin_base, start_off);
        const size_t ov_end   = std::min(lin_end,  end_off);

        if (ov_start < ov_end) {
            const size_t in_entry = ov_start - lin_base;
            const size_t n        = ov_end - ov_start;
            const size_t dst_off  = ov_start - start_off;

            const void* hva = ctx_.gpa_to_hva(e.gpa + in_entry);
            if (hva) std::memcpy(dst + dst_off, hva, n);
            else     std::memset(dst + dst_off, 0,   n);
            copied += n;
        }

        lin_base = lin_end;
    }
    return copied;
}

// ── Command dispatch ──────────────────────────────────────────────────────────

void Gpu::handle_command(const uint8_t* req, uint32_t req_len,
                          uint8_t* resp, uint32_t resp_cap,
                          uint32_t& resp_len) {
    if (req_len < sizeof(GpuCtrlHdr)) { resp_len = 0; return; }

    GpuCtrlHdr hdr{};
    std::memcpy(&hdr, req, sizeof(hdr));

    const auto ok_nodata = [&]() {
        GpuCtrlHdr r{};
        r.type = VIRTIO_GPU_RESP_OK_NODATA;
        if (hdr.flags & VIRTIO_GPU_FLAG_FENCE) {
            r.flags    = VIRTIO_GPU_FLAG_FENCE;
            r.fence_id = hdr.fence_id;
        }
        std::memcpy(resp, &r, sizeof(r));
        resp_len = sizeof(r);
    };

    const auto err = [&](uint32_t type) {
        GpuCtrlHdr r{};
        r.type = type;
        std::memcpy(resp, &r, sizeof(r));
        resp_len = sizeof(r);
    };

    switch (hdr.type) {

    case VIRTIO_GPU_CMD_GET_DISPLAY_INFO: {
        GpuRespDisplayInfo info{};
        info.hdr.type = VIRTIO_GPU_RESP_OK_DISPLAY_INFO;
        if (hdr.flags & VIRTIO_GPU_FLAG_FENCE) {
            info.hdr.flags    = VIRTIO_GPU_FLAG_FENCE;
            info.hdr.fence_id = hdr.fence_id;
        }
        info.pmodes[0].r       = {0, 0, width_, height_};
        info.pmodes[0].enabled = 1;
        const uint32_t copy = std::min(resp_cap, uint32_t(sizeof(info)));
        std::memcpy(resp, &info, copy);
        resp_len = copy;
        break;
    }

    case VIRTIO_GPU_CMD_RESOURCE_CREATE_2D: {
        if (req_len < sizeof(GpuResourceCreate2d)) {
            err(VIRTIO_GPU_RESP_ERR_UNSPEC); break;
        }
        GpuResourceCreate2d cmd{};
        std::memcpy(&cmd, req, sizeof(cmd));

        Resource res{};
        res.id      = cmd.resource_id;
        res.format  = cmd.format;
        res.width   = cmd.width;
        res.height  = cmd.height;
        res.host_buf.assign(size_t(cmd.width) * cmd.height * 4, 0);
        resources_[cmd.resource_id] = std::move(res);

        logger::log("[gpu] resource_create id=%u %ux%u fmt=%u\n",
                    cmd.resource_id, cmd.width, cmd.height, cmd.format);
        ok_nodata();
        break;
    }

    case VIRTIO_GPU_CMD_RESOURCE_UNREF: {
        if (req_len < sizeof(GpuResourceUnref)) {
            err(VIRTIO_GPU_RESP_ERR_UNSPEC); break;
        }
        GpuResourceUnref cmd{};
        std::memcpy(&cmd, req, sizeof(cmd));
        resources_.erase(cmd.resource_id);
        if (scanout_resource_id_ == cmd.resource_id)
            scanout_resource_id_ = 0;
        ok_nodata();
        break;
    }

    case VIRTIO_GPU_CMD_SET_SCANOUT: {
        if (req_len < sizeof(GpuSetScanout)) {
            err(VIRTIO_GPU_RESP_ERR_UNSPEC); break;
        }
        GpuSetScanout cmd{};
        std::memcpy(&cmd, req, sizeof(cmd));
        if (cmd.scanout_id == 0) {
            scanout_resource_id_ = cmd.resource_id;
            logger::log("[gpu] scanout 0 → resource %u\n", cmd.resource_id);
        }
        ok_nodata();
        break;
    }

    case VIRTIO_GPU_CMD_RESOURCE_FLUSH: {
        if (req_len < sizeof(GpuResourceFlush)) {
            err(VIRTIO_GPU_RESP_ERR_UNSPEC); break;
        }
        GpuResourceFlush cmd{};
        std::memcpy(&cmd, req, sizeof(cmd));

        Resource* res = find_resource(cmd.resource_id);
        if (!res) { err(VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID); break; }

        if (frame_cb_
            && cmd.resource_id == scanout_resource_id_
            && !res->host_buf.empty())
        {
            pending_frames_.push_back(
                {res->host_buf, res->width, res->height, res->width * 4u});
        }
        ok_nodata();
        break;
    }

    case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D: {
        if (req_len < sizeof(GpuTransferToHost2d)) {
            err(VIRTIO_GPU_RESP_ERR_UNSPEC); break;
        }
        GpuTransferToHost2d cmd{};
        std::memcpy(&cmd, req, sizeof(cmd));

        Resource* res = find_resource(cmd.resource_id);
        if (!res) { err(VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID); break; }

        const size_t total = size_t(res->width) * res->height * 4;
        if (res->host_buf.size() < total)
            res->host_buf.resize(total, 0);

        const uint32_t rx2 = std::min(cmd.r.x + cmd.r.width,  res->width);
        const uint32_t ry2 = std::min(cmd.r.y + cmd.r.height, res->height);

        if (cmd.r.x < rx2 && cmd.r.y < ry2) {
            const uint32_t res_stride = res->width * 4;
            const uint32_t row_bytes  = (rx2 - cmd.r.x) * 4;

            for (uint32_t row = cmd.r.y; row < ry2; ++row) {
                const size_t src_off =
                    static_cast<size_t>(cmd.offset)
                    + size_t(row - cmd.r.y) * res_stride;

                uint8_t* const dst_row =
                    res->host_buf.data()
                    + size_t(row)     * res_stride
                    + size_t(cmd.r.x) * 4;

                read_backing(*res, src_off, dst_row, row_bytes);
            }
        }
        ok_nodata();
        break;
    }

    case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING: {
        if (req_len < sizeof(GpuResourceAttachBacking)) {
            err(VIRTIO_GPU_RESP_ERR_UNSPEC); break;
        }
        GpuResourceAttachBacking cmd{};
        std::memcpy(&cmd, req, sizeof(cmd));

        Resource* res = find_resource(cmd.resource_id);
        if (!res) { err(VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID); break; }

        const size_t base = sizeof(GpuResourceAttachBacking);
        for (uint32_t i = 0; i < cmd.nr_entries; ++i) {
            const size_t off = base + size_t(i) * sizeof(GpuMemEntry);
            if (off + sizeof(GpuMemEntry) > req_len) break;
            GpuMemEntry entry{};
            std::memcpy(&entry, req + off, sizeof(entry));
            res->backing.push_back({entry.addr, entry.length});
        }
        logger::log("[gpu] resource %u attached %u backing pages\n",
                    cmd.resource_id, cmd.nr_entries);
        ok_nodata();
        break;
    }

    case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING: {
        if (req_len < sizeof(GpuResourceDetachBacking)) {
            err(VIRTIO_GPU_RESP_ERR_UNSPEC); break;
        }
        GpuResourceDetachBacking cmd{};
        std::memcpy(&cmd, req, sizeof(cmd));
        if (Resource* res = find_resource(cmd.resource_id))
            res->backing.clear();
        ok_nodata();
        break;
    }

    default:
        logger::log("[gpu] unknown virtio cmd 0x%x\n", hdr.type);
        err(VIRTIO_GPU_RESP_ERR_UNSPEC);
        break;
    }
}

// ── Control queue drain ───────────────────────────────────────────────────────

void Gpu::process_controlq() {
    Queue& q = queues_[0];
    if (!q.ready || !q.desc_gpa || !q.avail_gpa || !q.used_gpa) return;

    auto* desc  = static_cast<VDesc*> (ctx_.gpa_to_hva(q.desc_gpa));
    auto* avail = static_cast<VAvail*>(ctx_.gpa_to_hva(q.avail_gpa));
    auto* used  = static_cast<VUsed*> (ctx_.gpa_to_hva(q.used_gpa));
    if (!desc || !avail || !used) return;

    while (q.last_avail != avail->idx) {
        const uint16_t head = avail->ring[q.last_avail % kQueueSize];

        std::vector<uint8_t> req_buf;
        uint8_t* resp_ptr = nullptr;
        uint32_t  resp_cap = 0;

        uint16_t c = head;
        for (int step = 0; step < 64; ++step) {
            void* hva = ctx_.gpa_to_hva(desc[c].addr);
            if (!hva) break;

            if (desc[c].flags & VRING_DESC_F_WRITE) {
                resp_ptr = static_cast<uint8_t*>(hva);
                resp_cap = desc[c].len;
            } else {
                const auto* src = static_cast<const uint8_t*>(hva);
                req_buf.insert(req_buf.end(), src, src + desc[c].len);
            }

            if (!(desc[c].flags & VRING_DESC_F_NEXT)) break;
            c = desc[c].next;
        }

        uint32_t resp_len = 0;
        if (!req_buf.empty() && resp_ptr && resp_cap >= sizeof(GpuCtrlHdr))
            handle_command(req_buf.data(), uint32_t(req_buf.size()),
                           resp_ptr, resp_cap, resp_len);

        const uint16_t ui = used->idx % kQueueSize;
        used->ring[ui].id  = head;
        used->ring[ui].len = resp_len;
        __atomic_store_n(&used->idx,
                         static_cast<uint16_t>(used->idx + 1),
                         __ATOMIC_RELEASE);
        ++q.last_avail;
    }

    irq_status_ |= 1;
    notify_irq();
}

// ── MMIO ──────────────────────────────────────────────────────────────────────

uint32_t Gpu::read(uint32_t off, uint32_t /*len*/) {
    std::lock_guard<std::mutex> lk(mtx_);
    switch (off) {
        case Reg::Magic:    return 0x74726976U;
        case Reg::Version:  return 2;
        case Reg::DeviceId: return 16; // virtio-gpu
        case Reg::VendorId: return 0x554D4551U;

        case Reg::DeviceFeat:
            if (dev_feat_sel_ == 0) return static_cast<uint32_t>(device_features_);
            if (dev_feat_sel_ == 1) return static_cast<uint32_t>(device_features_ >> 32);
            return 0;

        case Reg::QueueNumMax: return kQueueSize;
        case Reg::QueueReady:  return queues_[queue_sel_].ready ? 1u : 0u;
        case Reg::IrqStatus:   return irq_status_;
        case Reg::Status:      return status_;

        // Virtio 1.2 Spec: Unsupported Shared Memory lengths MUST return ~0.
        // Returning 0 here causes the DRM driver to abort with -EBUSY.
        case Reg::ShmLenLo:    return 0xFFFFFFFF;
        case Reg::ShmLenHi:    return 0xFFFFFFFF;

        case Reg::ConfigGen:   return 0;
        case Reg::ConfigBase +  0: return 0; // events_read
        case Reg::ConfigBase +  4: return 0; // events_clear
        case Reg::ConfigBase +  8: return 1; // num_scanouts = 1
        case Reg::ConfigBase + 12: return 0; // num_capsets  = 0 (2D only)
        default:                   return 0;
    }
}

void Gpu::write(uint32_t off, uint32_t val, uint32_t /*len*/) {
    std::unique_lock<std::mutex> lk(mtx_);
    Queue& q = queues_[queue_sel_];

    switch (off) {
        case Reg::DeviceFeatSel: dev_feat_sel_ = val; break;
        case Reg::DriverFeatSel: drv_feat_sel_ = val; break;

        case Reg::DriverFeat:
            if (drv_feat_sel_ == 0)
                driver_features_ = (driver_features_ & 0xFFFFFFFF00000000ULL) | val;
            else if (drv_feat_sel_ == 1)
                driver_features_ = (driver_features_ & 0x00000000FFFFFFFFULL)
                                   | (static_cast<uint64_t>(val) << 32);
            break;

        case Reg::QueueSel:
            queue_sel_ = std::min(val, 1u);
            break;
        case Reg::QueueNum:
            q.num = std::min(val, kQueueSize);
            break;
        case Reg::QueueReady:
            q.ready = (val != 0);
            break;
        case Reg::QueueNotify:
            if (val == 0) process_controlq();
            break;
        case Reg::IrqAck:
            irq_status_ &= ~val;
            break;

        case Reg::Status: {
            const uint32_t prev = status_;
            status_ = val;
            if ((val & VIRTIO_STATUS_FEATURES_OK) &&
                !(prev & VIRTIO_STATUS_FEATURES_OK))
            {
                if (driver_features_ & ~device_features_) {
                    status_ &= ~VIRTIO_STATUS_FEATURES_OK;
                    logger::log("[gpu] FEATURES_OK refused: "
                                "driver=0x%llx device=0x%llx\n",
                                static_cast<unsigned long long>(driver_features_),
                                static_cast<unsigned long long>(device_features_));
                }
            }
            if (val == 0) {
                queues_[0] = queues_[1] = Queue{};
                irq_status_          = 0;
                scanout_resource_id_ = 0;
                resources_.clear();
                driver_features_     = 0;
                pending_frames_.clear();
            }
            break;
        }

        case Reg::QueueDescLo:
            q.desc_gpa  = (q.desc_gpa  & 0xFFFFFFFF00000000ULL) | val; break;
        case Reg::QueueDescHi:
            q.desc_gpa  = (q.desc_gpa  & 0x00000000FFFFFFFFULL)
                          | (static_cast<uint64_t>(val) << 32); break;
        case Reg::QueueAvailLo:
            q.avail_gpa = (q.avail_gpa & 0xFFFFFFFF00000000ULL) | val; break;
        case Reg::QueueAvailHi:
            q.avail_gpa = (q.avail_gpa & 0x00000000FFFFFFFFULL)
                          | (static_cast<uint64_t>(val) << 32); break;
        case Reg::QueueUsedLo:
            q.used_gpa  = (q.used_gpa  & 0xFFFFFFFF00000000ULL) | val; break;
        case Reg::QueueUsedHi:
            q.used_gpa  = (q.used_gpa  & 0x00000000FFFFFFFFULL)
                          | (static_cast<uint64_t>(val) << 32); break;
        default: break;
    }

    // Drain staged frames after releasing the lock to prevent deadlock
    std::vector<PendingFrame> frames;
    frames.swap(pending_frames_);
    lk.unlock();

    if (frame_cb_) {
        for (auto& f : frames)
            frame_cb_(f.pixels.data(), f.width, f.height, f.stride);
    }
}

} // namespace virtualization::virtio