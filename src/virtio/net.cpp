#include "virtualization/virtio/net.hpp"
#include "virtualization/firmware/arm64/gic.hpp"
#include "virtualization/vm_context.hpp"
#include "virtualization/logger.hpp"
#include "virtualization/network/network.hpp"
#include <algorithm>
#include <cstring>

namespace virtualization::virtio {

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
    inline constexpr uint32_t ConfigGen     = 0x0FC;
    inline constexpr uint32_t ConfigBase    = 0x100;
}

inline constexpr uint64_t kVirtioF_Version1 = 1ULL << 32;
inline constexpr uint64_t kVirtioNetF_Mac   = 1ULL <<  5;

// ── Constructor / Destructor ──────────────────────────────────────────────────

Net::Net(VMContext& ctx, uint64_t /*gpa*/, const NetDeviceConfig& cfg) noexcept
    : ctx_(ctx)
    , features_(kVirtioF_Version1 | kVirtioNetF_Mac)
{
    std::memcpy(mac_, cfg.mac, 6);

    network::Config nc;
    std::memcpy(nc.guest_mac, cfg.mac, 6);
    for (const auto& pf : cfg.port_forwards)
        nc.port_forwards.push_back({pf.host_port, pf.guest_port});

    stack_ = std::make_unique<network::Stack>(nc,
        [this](const uint8_t* frame, std::size_t len) {
            enqueue_rx(frame, len);
        });

    rx_flush_thread_ = std::thread(&Net::rx_flush_func, this);
}

Net::~Net() {
    stack_.reset();
    {
        std::lock_guard<std::mutex> lk(rx_mtx_);
        rx_stop_.store(true, std::memory_order_relaxed);
        rx_cv_.notify_all();
    }
    if (rx_flush_thread_.joinable()) rx_flush_thread_.join();
}

// ── IRQ ───────────────────────────────────────────────────────────────────────

void Net::notify_irq() { raise_spi(kSpiNet); }

// ── RX path ───────────────────────────────────────────────────────────────────

void Net::enqueue_rx(const uint8_t* frame, std::size_t len) {
    std::lock_guard<std::mutex> lk(rx_mtx_);
    rx_queue_.emplace(frame, frame + len);
    rx_cv_.notify_one();
}

void Net::rx_flush_func() {
    while (true) {
        std::vector<std::vector<uint8_t>> batch;
        {
            std::unique_lock<std::mutex> lk(rx_mtx_);
            rx_cv_.wait(lk, [this] {
                return !rx_queue_.empty() ||
                        rx_stop_.load(std::memory_order_relaxed);
            });
            if (rx_stop_.load() && rx_queue_.empty()) break;
            while (!rx_queue_.empty()) {
                batch.push_back(std::move(rx_queue_.front()));
                rx_queue_.pop();
            }
        }

        std::lock_guard<std::mutex> lk(mtx_);
        for (const auto& f : batch)
            do_feed_rx(f.data(), f.size());
    }
}

// Called with mtx_ held.
void Net::do_feed_rx(const uint8_t* frame, std::size_t flen) {
    Queue& q = queues_[0];
    if (!q.ready || !q.desc_gpa || !q.avail_gpa || !q.used_gpa) return;

    auto* desc  = static_cast<VDesc*> (ctx_.gpa_to_hva(q.desc_gpa));
    auto* avail = static_cast<VAvail*>(ctx_.gpa_to_hva(q.avail_gpa));
    auto* used  = static_cast<VUsed*> (ctx_.gpa_to_hva(q.used_gpa));
    if (!desc || !avail || !used || q.last_avail == avail->idx) return;

    const uint16_t head = avail->ring[q.last_avail % kQueueSize];
    void*    buf = ctx_.gpa_to_hva(desc[head].addr);
    uint32_t cap = desc[head].len;

    const std::size_t hdr_sz = sizeof(VirtioNetHdr);
    if (!buf || cap < static_cast<uint32_t>(flen + hdr_sz)) return;

    // 1. Write a zeroed virtio-net header (num_buffers = 1 per spec §5.1.6).
    std::memset(buf, 0, hdr_sz);
    static_cast<VirtioNetHdr*>(buf)->num_buffers = 1;

    // 2. Copy the Ethernet frame.
    std::memcpy(static_cast<uint8_t*>(buf) + hdr_sz, frame, flen);

    const uint16_t ui = used->idx % kQueueSize;
    used->ring[ui].id  = head;
    used->ring[ui].len = static_cast<uint32_t>(flen + hdr_sz);
    __atomic_store_n(&used->idx,
                     static_cast<uint16_t>(used->idx + 1),
                     __ATOMIC_RELEASE);
    ++q.last_avail;

    irq_status_ |= 1;
    notify_irq();
}

// ── TX path ───────────────────────────────────────────────────────────────────

void Net::process_tx() {
    Queue& q = queues_[1];
    if (!q.ready || !q.desc_gpa || !q.avail_gpa || !q.used_gpa) return;

    auto* desc  = static_cast<VDesc*> (ctx_.gpa_to_hva(q.desc_gpa));
    auto* avail = static_cast<VAvail*>(ctx_.gpa_to_hva(q.avail_gpa));
    auto* used  = static_cast<VUsed*> (ctx_.gpa_to_hva(q.used_gpa));
    if (!desc || !avail || !used) return;

    while (q.last_avail != avail->idx) {
        const uint16_t head = avail->ring[q.last_avail % kQueueSize];

        std::vector<uint8_t> frame;
        uint16_t cur = head;
        do {
            if (void* b = ctx_.gpa_to_hva(desc[cur].addr); b && desc[cur].len)
                frame.insert(frame.end(),
                             static_cast<uint8_t*>(b),
                             static_cast<uint8_t*>(b) + desc[cur].len);
            if (!(desc[cur].flags & 1)) break;
            cur = desc[cur].next;
        } while (true);

        const std::size_t hdr_sz = sizeof(VirtioNetHdr);
        if (frame.size() > hdr_sz)
            stack_->guest_tx(frame.data() + hdr_sz, frame.size() - hdr_sz);

        const uint16_t ui = used->idx % kQueueSize;
        used->ring[ui].id  = head;
        used->ring[ui].len = 0;
        __atomic_store_n(&used->idx,
                         static_cast<uint16_t>(used->idx + 1),
                         __ATOMIC_RELEASE);
        ++q.last_avail;
    }

    irq_status_ |= 1;
    notify_irq();
}

// ── MMIO ─────────────────────────────────────────────────────────────────────

uint32_t Net::read(uint32_t off, uint32_t len) {
    std::lock_guard<std::mutex> lk(mtx_);
    const Queue* q = (queue_sel_ < 2) ? &queues_[queue_sel_] : nullptr;

    // MAC address config space (6 bytes at ConfigBase)
    if (off >= Reg::ConfigBase && off < Reg::ConfigBase + 6) {
        uint32_t val = 0;
        for (uint32_t i = 0; i < len && (off - Reg::ConfigBase + i) < 6; ++i)
            val |= static_cast<uint32_t>(mac_[off - Reg::ConfigBase + i]) << (i * 8);
        return val;
    }

    switch (off) {
        case Reg::Magic:       return 0x74726976U;
        case Reg::Version:     return 2;
        case Reg::DeviceId:    return 1; // virtio-net
        case Reg::VendorId:    return 0x554D4551U;
        case Reg::DeviceFeat:
            if (dev_feat_sel_ == 0) return static_cast<uint32_t>(features_);
            if (dev_feat_sel_ == 1) return static_cast<uint32_t>(features_ >> 32);
            return 0;
        case Reg::QueueNumMax: return kQueueSize;
        case Reg::QueueReady:  return (q && q->ready) ? 1u : 0u;
        case Reg::IrqStatus:   return irq_status_;
        case Reg::Status:      return status_;
        case Reg::ConfigGen:   return 0;
        default:               return 0;
    }
}

void Net::write(uint32_t off, uint32_t val, uint32_t /*len*/) {
    std::lock_guard<std::mutex> lk(mtx_);
    Queue* q = (queue_sel_ < 2) ? &queues_[queue_sel_] : nullptr;

#define LO(f, v) if(q) q->f = (q->f & 0xFFFFFFFF00000000ULL) | (v)
#define HI(f, v) if(q) q->f = (q->f & 0x00000000FFFFFFFFULL) | (static_cast<uint64_t>(v) << 32)

    switch (off) {
        case Reg::DeviceFeatSel: dev_feat_sel_ = val; break;
        case Reg::DriverFeatSel: drv_feat_sel_ = val; break;
        case Reg::DriverFeat:
            if (drv_feat_sel_ == 0)
                features_ = (features_ & 0xFFFFFFFF00000000ULL) | val;
            else if (drv_feat_sel_ == 1)
                features_ = (features_ & 0x00000000FFFFFFFFULL)
                            | (static_cast<uint64_t>(val) << 32);
            break;
        case Reg::QueueSel:    queue_sel_ = val; break;
        case Reg::QueueNum:    if (q) q->num = std::min(val, kQueueSize); break;
        case Reg::QueueReady:  if (q) q->ready = (val != 0); break;
        case Reg::QueueNotify: if (val == 1) process_tx(); break;
        case Reg::IrqAck:      irq_status_ &= ~val; break;
        case Reg::Status:
            status_ = val;
            if (val == 0) {
                for (auto& queue : queues_) queue = Queue{};
                irq_status_ = 0;
            }
            break;
        case Reg::QueueDescLo:  LO(desc_gpa,  val); break;
        case Reg::QueueDescHi:  HI(desc_gpa,  val); break;
        case Reg::QueueAvailLo: LO(avail_gpa, val); break;
        case Reg::QueueAvailHi: HI(avail_gpa, val); break;
        case Reg::QueueUsedLo:  LO(used_gpa,  val); break;
        case Reg::QueueUsedHi:  HI(used_gpa,  val); break;
        default: break;
    }

#undef LO
#undef HI
}

} // namespace virtualization::virtio