#pragma once
#include "virtualization/vm_context.hpp"
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace virtualization::virtio {

/// Virtio 1.0 MMIO GPU device — 2D framebuffer mode.
///
/// ### Display path
/// The guest kernel DRM virtio_gpu driver allocates backing memory for
/// 2D resources. It issues VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D to push
/// rendered pixel data into the host's resource buffer, followed by
/// VIRTIO_GPU_CMD_RESOURCE_FLUSH to display it.
/// 
/// The frame_cb_ is fired directly on the vCPU thread that handles the
/// virtio MMIO register write, *after* releasing the internal mutex to
/// prevent deadlock with UI consumers.
class Gpu {
public:
    /// Signature: (pixels, width, height, stride_bytes)
    using FrameCallback = std::function<void(const uint8_t* pixels,
                                             uint32_t       width,
                                             uint32_t       height,
                                             uint32_t       stride)>;

    Gpu(VMContext&    ctx,
        uint64_t      mmio_gpa,
        uint32_t      width,
        uint32_t      height,
        FrameCallback cb) noexcept;
    ~Gpu() = default;

    Gpu(const Gpu&)            = delete;
    Gpu& operator=(const Gpu&) = delete;
    Gpu(Gpu&&)                 = delete;
    Gpu& operator=(Gpu&&)      = delete;

    // ── Virtio MMIO interface ─────────────────────────────────────
    [[nodiscard]] uint32_t read (uint32_t off, uint32_t len);
    void                   write(uint32_t off, uint32_t val, uint32_t len);
    void                   notify_irq();

    // ── Accessors ─────────────────────────────────────────────────
    [[nodiscard]] uint32_t width()   const noexcept { return width_;   }
    [[nodiscard]] uint32_t height()  const noexcept { return height_;  }

private:
    static constexpr uint32_t kQueueSize = 256;

    // ── Virtio ring structures ────────────────────────────────────
    struct VDesc {
        uint64_t addr;
        uint32_t len;
        uint16_t flags;
        uint16_t next;
    } __attribute__((packed));

    struct VAvail    { uint16_t flags, idx; uint16_t ring[kQueueSize]; };
    struct VUsedElem { uint32_t id, len; };
    struct VUsed     { uint16_t flags, idx; VUsedElem ring[kQueueSize]; };

    struct Queue {
        uint32_t num        {kQueueSize};
        uint64_t desc_gpa   {0};
        uint64_t avail_gpa  {0};
        uint64_t used_gpa   {0};
        uint16_t last_avail {0};
        bool     ready      {false};
    };

    // ── Per-resource guest-backing descriptor ─────────────────────
    struct MemEntry { uint64_t gpa; uint32_t len; };

    struct Resource {
        uint32_t              id      {0};
        uint32_t              format  {0};
        uint32_t              width   {0};
        uint32_t              height  {0};
        std::vector<MemEntry> backing;
        std::vector<uint8_t>  host_buf; ///< Assembled pixel data (BGRA8)
    };

    /// Frame staged by handle_command() inside the lock; delivered by
    /// write() after the lock is released.
    struct PendingFrame {
        std::vector<uint8_t> pixels;
        uint32_t             width;
        uint32_t             height;
        uint32_t             stride;
    };

    // ── Internal helpers ──────────────────────────────────────────
    void       process_controlq();
    void       handle_command(const uint8_t* req, uint32_t req_len,
                               uint8_t* resp, uint32_t resp_cap,
                               uint32_t& resp_len);
    Resource* find_resource(uint32_t id);
    size_t     read_backing(const Resource& res,
                             size_t start_off, uint8_t* dst, size_t len) const;

    // ── State ─────────────────────────────────────────────────────
    VMContext&    ctx_;
    std::mutex    mtx_;
    FrameCallback frame_cb_;

    uint32_t width_;
    uint32_t height_;

    // ── Virtio state ──────────────────────────────────────────────
    uint32_t status_       {0};
    uint32_t queue_sel_    {0};
    uint32_t irq_status_   {0};
    uint32_t dev_feat_sel_ {0};
    uint32_t drv_feat_sel_ {0};

    uint64_t device_features_;    ///< Constant after construction: what we offer
    uint64_t driver_features_{0}; ///< Written by the driver during negotiation

    Queue queues_[2]; ///< [0] = controlq, [1] = cursorq (cursorq ignored in 2D)

    uint32_t scanout_resource_id_ {0};

    std::unordered_map<uint32_t, Resource> resources_;

    std::vector<PendingFrame> pending_frames_;
};

} // namespace virtualization::virtio