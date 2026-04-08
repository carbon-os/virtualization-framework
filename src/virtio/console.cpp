#include "virtualization/virtio/console.hpp"
#include "virtualization/firmware/arm64/gic.hpp"
#include "virtualization/vm_context.hpp"
#include "virtualization/logger.hpp"
#include <algorithm>
#include <cstring>
#include <poll.h>
#include <unistd.h>

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
}

inline constexpr uint64_t kVirtioF_Version1 = 1ULL << 32;

// ── Constructor / Destructor ──────────────────────────────────────────────────

Console::Console(VMContext& ctx, uint64_t /*gpa*/) noexcept
    : ctx_(ctx), features_(kVirtioF_Version1)
{
    if (tcgetattr(STDIN_FILENO, &saved_termios_) == 0) {
        termios_saved_ = true;
        struct termios raw = saved_termios_;
        raw.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON | IEXTEN);
        raw.c_iflag &= ~static_cast<tcflag_t>(ICRNL | IXON | INPCK | ISTRIP);
        raw.c_oflag &= ~static_cast<tcflag_t>(OPOST);
        raw.c_cc[VMIN]  = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
    rx_thread_ = std::thread(&Console::rx_thread_func, this);
}

Console::~Console() {
    stop_rx_.store(true, std::memory_order_relaxed);
    if (rx_thread_.joinable()) rx_thread_.join();
    if (termios_saved_)
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios_);
}

// ── IRQ ───────────────────────────────────────────────────────────────────────

void Console::notify_irq() { raise_spi(kSpiConsole); }

// ── RX stdin thread ───────────────────────────────────────────────────────────

void Console::rx_thread_func() {
    struct pollfd pfd {STDIN_FILENO, POLLIN, 0};
    while (!stop_rx_.load(std::memory_order_relaxed)) {
        if (::poll(&pfd, 1, 20) > 0 && (pfd.revents & POLLIN)) {
            std::lock_guard<std::mutex> lk(mtx_);
            feed_rx();
        }
    }
}

// Called with mtx_ held.
void Console::feed_rx() {
    Queue& q = queues_[0];
    if (!q.ready || !q.desc_gpa || !q.avail_gpa || !q.used_gpa) return;

    auto* desc  = static_cast<VDesc*> (ctx_.gpa_to_hva(q.desc_gpa));
    auto* avail = static_cast<VAvail*>(ctx_.gpa_to_hva(q.avail_gpa));
    auto* used  = static_cast<VUsed*> (ctx_.gpa_to_hva(q.used_gpa));
    if (!desc || !avail || !used || q.last_avail == avail->idx) return;

    const uint16_t head = avail->ring[q.last_avail % kQueueSize];
    void*    buf = ctx_.gpa_to_hva(desc[head].addr);
    uint32_t cap = desc[head].len;
    if (!buf || !cap) return;

    char c;
    if (::read(STDIN_FILENO, &c, 1) <= 0) return;

    *static_cast<char*>(buf) = c;

    const uint16_t ui = used->idx % kQueueSize;
    used->ring[ui].id  = head;
    used->ring[ui].len = 1;
    __atomic_store_n(&used->idx,
                     static_cast<uint16_t>(used->idx + 1),
                     __ATOMIC_RELEASE);
    ++q.last_avail;

    irq_status_ |= 1;
    notify_irq();
}

// ── TX — guest → host ─────────────────────────────────────────────────────────

void Console::process_tx() {
    Queue& q = queues_[1];
    if (!q.ready || !q.desc_gpa || !q.avail_gpa || !q.used_gpa) return;

    auto* desc  = static_cast<VDesc*> (ctx_.gpa_to_hva(q.desc_gpa));
    auto* avail = static_cast<VAvail*>(ctx_.gpa_to_hva(q.avail_gpa));
    auto* used  = static_cast<VUsed*> (ctx_.gpa_to_hva(q.used_gpa));
    if (!desc || !avail || !used) return;

    while (q.last_avail != avail->idx) {
        const uint16_t head  = avail->ring[q.last_avail % kQueueSize];
        uint32_t       wrote = 0;
        uint16_t       cur   = head;

        do {
            if (void* buf = ctx_.gpa_to_hva(desc[cur].addr);
                buf && desc[cur].len) {
                logger::write(static_cast<const char*>(buf), desc[cur].len);
                wrote += desc[cur].len;
            }
            if (!(desc[cur].flags & 1)) break;
            cur = desc[cur].next;
        } while (true);

        const uint16_t ui = used->idx % kQueueSize;
        used->ring[ui].id  = head;
        used->ring[ui].len = wrote;
        __atomic_store_n(&used->idx,
                         static_cast<uint16_t>(used->idx + 1),
                         __ATOMIC_RELEASE);
        ++q.last_avail;
    }

    irq_status_ |= 1;
    notify_irq();
}

// ── MMIO ─────────────────────────────────────────────────────────────────────

uint32_t Console::read(uint32_t off, uint32_t /*len*/) {
    std::lock_guard<std::mutex> lk(mtx_);
    const Queue* q = (queue_sel_ < 2) ? &queues_[queue_sel_] : nullptr;
    switch (off) {
        case Reg::Magic:       return 0x74726976U;
        case Reg::Version:     return 2;
        case Reg::DeviceId:    return 3; // virtio-console
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

void Console::write(uint32_t off, uint32_t val, uint32_t /*len*/) {
    std::lock_guard<std::mutex> lk(mtx_);
    Queue* q = (queue_sel_ < 2) ? &queues_[queue_sel_] : nullptr;

#define LO(field, v) if(q) q->field = (q->field & 0xFFFFFFFF00000000ULL) | (v)
#define HI(field, v) if(q) q->field = (q->field & 0x00000000FFFFFFFFULL) | (static_cast<uint64_t>(v) << 32)

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