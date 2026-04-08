#pragma once
#include "virtualization/vm_context.hpp"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <termios.h>
#include <thread>

namespace virtualization::virtio {

/// Virtio 1.0 MMIO console device.
///
/// Queue 0 — RX (host → guest): a background thread polls stdin with a
///   20 ms timeout and feeds each byte into the guest's receive buffer.
/// Queue 1 — TX (guest → host): guest output is forwarded to logger::write.
///
/// The host terminal is placed in raw mode on construction and restored
/// on destruction.
class Console {
public:
    Console(VMContext& ctx, uint64_t gpa) noexcept;
    ~Console();

    Console(const Console&)            = delete;
    Console& operator=(const Console&) = delete;
    Console(Console&&)                 = delete;
    Console& operator=(Console&&)      = delete;

    [[nodiscard]] uint32_t read (uint32_t off, uint32_t len);
    void                   write(uint32_t off, uint32_t val, uint32_t len);
    void                   notify_irq();

private:
    static constexpr uint32_t kQueueSize = 256;

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

    /// Guest → host output.  Called with mtx_ held.
    void process_tx();

    /// Host → guest one byte from stdin.  Called with mtx_ held.
    void feed_rx();

    /// Entry point for the RX stdin polling thread.
    void rx_thread_func();

    VMContext& ctx_;
    std::mutex mtx_;

    uint32_t status_       {0};
    uint32_t queue_sel_    {0};
    uint32_t irq_status_   {0};
    uint32_t dev_feat_sel_ {0};
    uint32_t drv_feat_sel_ {0};
    uint64_t features_;

    Queue queues_[2]; ///< [0] = RX, [1] = TX

    std::atomic<bool> stop_rx_ {false};
    std::thread       rx_thread_;

    struct termios saved_termios_ {};
    bool           termios_saved_ {false};
};

} // namespace virtualization::virtio