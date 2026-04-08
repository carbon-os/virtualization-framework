#pragma once
#include "virtualization/vm_context.hpp"
#include "virtualization/network/network.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace virtualization::virtio {

/// Virtio 1.0 MMIO network device with a userspace NAT stack.
///
/// Queue 0 — RX (stack → guest): frames arrive on a netdev callback,
///   are queued, and a dedicated flush thread delivers them.
/// Queue 1 — TX (guest → stack): the exit handler notifies on
///   QueueNotify; frames are stripped of the virtio-net header and
///   handed to the stack.
class Net {
public:
    Net(VMContext& ctx, uint64_t gpa, const NetDeviceConfig& cfg) noexcept;
    ~Net();

    Net(const Net&)            = delete;
    Net& operator=(const Net&) = delete;
    Net(Net&&)                 = delete;
    Net& operator=(Net&&)      = delete;

    [[nodiscard]] uint32_t read (uint32_t off, uint32_t len);
    void                   write(uint32_t off, uint32_t val, uint32_t len);
    void                   notify_irq();

private:
    static constexpr uint32_t kQueueSize = 256;

    // Virtio-net header (v1.0 spec §5.1.6)
    struct VirtioNetHdr {
        uint8_t  flags;
        uint8_t  gso_type;
        uint16_t hdr_len;
        uint16_t gso_size;
        uint16_t csum_start;
        uint16_t csum_offset;
        uint16_t num_buffers; // must be 1 for non-mergeable RX
    } __attribute__((packed));

    struct VDesc {
        uint64_t addr;
        uint32_t len;
        uint16_t flags;
        uint16_t next;
    } __attribute__((packed));

    struct VAvail { uint16_t flags, idx; uint16_t ring[kQueueSize]; };
    struct VUsedElem { uint32_t id, len; };
    struct VUsed  { uint16_t flags, idx; VUsedElem ring[kQueueSize]; };

    struct Queue {
        uint32_t num        {kQueueSize};
        uint64_t desc_gpa   {0};
        uint64_t avail_gpa  {0};
        uint64_t used_gpa   {0};
        uint16_t last_avail {0};
        bool     ready      {false};
    };

    /// Called from the network stack callback; enqueues onto rx_queue_.
    void enqueue_rx(const uint8_t* frame, std::size_t len);

    /// Entry point for the RX flush thread.
    void rx_flush_func();

    /// Deliver one Ethernet frame into the guest RX queue.
    /// Must be called with mtx_ held.
    void do_feed_rx(const uint8_t* frame, std::size_t len);

    /// Drain the guest TX queue and send frames via the network stack.
    /// Must be called with mtx_ held.
    void process_tx();

    VMContext& ctx_;
    std::mutex mtx_;

    uint32_t status_       {0};
    uint32_t queue_sel_    {0};
    uint32_t irq_status_   {0};
    uint32_t dev_feat_sel_ {0};
    uint32_t drv_feat_sel_ {0};
    uint64_t features_;
    uint8_t  mac_[6];

    Queue queues_[2]; ///< [0] = RX, [1] = TX

    std::unique_ptr<network::Stack>  stack_;
    std::mutex                       rx_mtx_;
    std::condition_variable          rx_cv_;
    std::queue<std::vector<uint8_t>> rx_queue_;
    std::atomic<bool>                rx_stop_ {false};
    std::thread                      rx_flush_thread_;
};

} // namespace virtualization::virtio