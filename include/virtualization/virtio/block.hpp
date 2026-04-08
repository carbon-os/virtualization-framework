#pragma once
#include "virtualization/vm_context.hpp"
#include <cstdint>
#include <mutex>

namespace virtualization::virtio {

/// Virtio 1.0 MMIO block device backed by a raw disk image.
/// Supports IN, OUT, FLUSH, and GET_ID request types.
class Block {
public:
    Block(VMContext& ctx, uint64_t gpa, const char* image_path) noexcept;
    ~Block();

    Block(const Block&)            = delete;
    Block& operator=(const Block&) = delete;
    Block(Block&&)                 = delete;
    Block& operator=(Block&&)      = delete;

    /// True if the image file was opened successfully.
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

    [[nodiscard]] uint32_t read (uint32_t off, uint32_t len);
    void                   write(uint32_t off, uint32_t val, uint32_t len);
    void                   notify_irq();

private:
    static constexpr uint32_t kQueueSize = 128;

    struct VDesc {
        uint64_t addr;
        uint32_t len;
        uint16_t flags;
        uint16_t next;
    } __attribute__((packed));

    struct VAvail {
        uint16_t flags, idx;
        uint16_t ring[kQueueSize];
    };

    struct VUsedElem { uint32_t id, len; };

    struct VUsed {
        uint16_t  flags, idx;
        VUsedElem ring[kQueueSize];
    };

    struct Queue {
        uint32_t num        {kQueueSize};
        uint64_t desc_gpa   {0};
        uint64_t avail_gpa  {0};
        uint64_t used_gpa   {0};
        uint16_t last_avail {0};
        bool     ready      {false};
    };

    /// Drains the available ring and services each request.
    /// Must be called with mtx_ held.
    void process_rq();

    VMContext& ctx_;
    std::mutex mtx_;

    int      fd_           {-1};
    uint64_t capacity_     {0};   ///< Disk capacity in 512-byte sectors.
    uint32_t status_       {0};
    uint32_t irq_status_   {0};
    uint32_t dev_feat_sel_ {0};
    uint32_t drv_feat_sel_ {0};
    uint64_t features_;
    Queue    queue_;
};

} // namespace virtualization::virtio