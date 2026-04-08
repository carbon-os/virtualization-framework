#include "virtualization/virtio/input.hpp"
#include "virtualization/firmware/arm64/gic.hpp"
#include "virtualization/vm_context.hpp"
#include "virtualization/logger.hpp"
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
    inline constexpr uint32_t ConfigBase    = 0x100;
}

inline constexpr uint64_t kVirtioF_Version1 = 1ULL << 32;
inline constexpr uint32_t kVirtioIdInput    = 18;

inline constexpr uint8_t VIRTIO_INPUT_CFG_UNSET    = 0x00;
inline constexpr uint8_t VIRTIO_INPUT_CFG_ID_NAME  = 0x01;
inline constexpr uint8_t VIRTIO_INPUT_CFG_EV_BITS  = 0x11;
inline constexpr uint8_t VIRTIO_INPUT_CFG_ABS_INFO = 0x12;

struct virtio_input_absinfo {
    uint32_t min;
    uint32_t max;
    uint32_t fuzz;
    uint32_t flat;
    uint32_t res;
} __attribute__((packed));

inline constexpr uint16_t VRING_DESC_F_WRITE = 2;

// ── Constructor ───────────────────────────────────────────────────────────────
// spi_id is accepted directly — the old formula (kSpiConsole + slot_index)
// produced kSpiConsole+4 = 20 = kSpiGpu, which corrupted GIC state and
// prevented the block device's SPI 17 completions from being delivered.

Input::Input(VMContext& ctx, uint64_t mmio_gpa, uint32_t spi_id, Type type,
             uint32_t abs_width, uint32_t abs_height) noexcept
    : ctx_(ctx)
    , type_(type)
    , abs_width_(abs_width)
    , abs_height_(abs_height)
    , spi_id_(spi_id)
    , device_features_(kVirtioF_Version1)
{
    (void)mmio_gpa; // retained in signature for symmetry with other virtio ctors
    logger::log("[input] virtio-input initialized: %s  SPI=%u\n",
                type_ == Type::Keyboard ? "Keyboard" : "Tablet", spi_id_);
    update_config();
}

void Input::notify_irq() { raise_spi(spi_id_); }

// ── Config Multiplexer ────────────────────────────────────────────────────────

void Input::update_config() {
    const uint8_t select = config_[0];
    const uint8_t subsel = config_[1];

    config_[2] = 0;
    std::memset(&config_[8], 0, 128);

    if (select == VIRTIO_INPUT_CFG_ID_NAME) {
        const char*   name = (type_ == Type::Keyboard) ? "virtio-keyboard" : "virtio-tablet";
        const uint8_t len  = static_cast<uint8_t>(std::strlen(name));
        std::memcpy(&config_[8], name, len);
        config_[2] = len;

    } else if (select == VIRTIO_INPUT_CFG_EV_BITS) {
        if (subsel == EV_SYN) {
            config_[8] = (1 << SYN_REPORT);
            config_[2] = 1;
        } else if (subsel == EV_KEY) {
            if (type_ == Type::Keyboard) {
                std::memset(&config_[8], 0xFF, 128);
                config_[2] = 128;
            } else if (type_ == Type::Tablet) {
                const int left_byte   = BTN_LEFT   / 8;
                const int right_byte  = BTN_RIGHT  / 8;
                const int middle_byte = BTN_MIDDLE / 8;
                config_[8 + left_byte]   |= (1 << (BTN_LEFT   % 8));
                config_[8 + right_byte]  |= (1 << (BTN_RIGHT  % 8));
                config_[8 + middle_byte] |= (1 << (BTN_MIDDLE % 8));
                config_[2] = static_cast<uint8_t>(middle_byte + 1);
            }
        } else if (subsel == EV_ABS && type_ == Type::Tablet) {
            config_[8] = (1 << ABS_X) | (1 << ABS_Y);
            config_[2] = 1;
        }

    } else if (select == VIRTIO_INPUT_CFG_ABS_INFO && type_ == Type::Tablet) {
        virtio_input_absinfo info{};
        if (subsel == ABS_X) {
            info.max = abs_width_;
            std::memcpy(&config_[8], &info, sizeof(info));
            config_[2] = sizeof(info);
        } else if (subsel == ABS_Y) {
            info.max = abs_height_;
            std::memcpy(&config_[8], &info, sizeof(info));
            config_[2] = sizeof(info);
        }
    }
}

// ── Event Injection ───────────────────────────────────────────────────────────

void Input::send_key(uint16_t linux_keycode, bool pressed) {
    std::lock_guard<std::mutex> lk(mtx_);
    push_event_locked(EV_KEY, linux_keycode, pressed ? 1 : 0);
    push_event_locked(EV_SYN, SYN_REPORT, 0);
    flush_events_locked();
}

void Input::send_pointer(uint32_t x, uint32_t y, bool btn_left, bool btn_right) {
    std::lock_guard<std::mutex> lk(mtx_);
    push_event_locked(EV_ABS, ABS_X, x);
    push_event_locked(EV_ABS, ABS_Y, y);
    push_event_locked(EV_KEY, BTN_LEFT,  btn_left  ? 1 : 0);
    push_event_locked(EV_KEY, BTN_RIGHT, btn_right ? 1 : 0);
    push_event_locked(EV_SYN, SYN_REPORT, 0);
    flush_events_locked();
}

void Input::push_event_locked(uint16_t type, uint16_t code, uint32_t value) {
    pending_events_.push_back({type, code, value});
    if (pending_events_.size() > kQueueSize * 2)
        pending_events_.pop_front();
}

void Input::flush_events_locked() {
    Queue& q = queues_[0];
    if (!q.ready || !q.avail_gpa || !q.used_gpa) return;

    auto* avail = static_cast<VAvail*>(ctx_.gpa_to_hva(q.avail_gpa));
    auto* used  = static_cast<VUsed*> (ctx_.gpa_to_hva(q.used_gpa));
    auto* desc  = static_cast<VDesc*> (ctx_.gpa_to_hva(q.desc_gpa));
    if (!avail || !used || !desc) return;

    bool injected = false;

    while (!pending_events_.empty() && q.last_avail != avail->idx) {
        const uint16_t head = avail->ring[q.last_avail % kQueueSize];
        void* hva = ctx_.gpa_to_hva(desc[head].addr);

        if (hva && (desc[head].flags & VRING_DESC_F_WRITE) &&
            desc[head].len >= sizeof(virtio_input_event))
        {
            virtio_input_event ev = pending_events_.front();
            pending_events_.pop_front();
            std::memcpy(hva, &ev, sizeof(virtio_input_event));

            const uint16_t ui = used->idx % kQueueSize;
            used->ring[ui].id  = head;
            used->ring[ui].len = sizeof(virtio_input_event);
            __atomic_store_n(&used->idx,
                             static_cast<uint16_t>(used->idx + 1),
                             __ATOMIC_RELEASE);
            injected = true;
        }
        ++q.last_avail;
    }

    if (injected) {
        irq_status_ |= 1;
        notify_irq();
    }
}

void Input::process_statusq() {
    Queue& q = queues_[1];
    if (!q.ready || !q.avail_gpa || !q.used_gpa) return;

    auto* avail = static_cast<VAvail*>(ctx_.gpa_to_hva(q.avail_gpa));
    auto* used  = static_cast<VUsed*> (ctx_.gpa_to_hva(q.used_gpa));
    if (!avail || !used) return;

    bool drained = false;
    while (q.last_avail != avail->idx) {
        const uint16_t head = avail->ring[q.last_avail % kQueueSize];
        const uint16_t ui   = used->idx % kQueueSize;
        used->ring[ui].id   = head;
        used->ring[ui].len  = 0;
        __atomic_store_n(&used->idx,
                         static_cast<uint16_t>(used->idx + 1),
                         __ATOMIC_RELEASE);
        ++q.last_avail;
        drained = true;
    }

    if (drained) {
        irq_status_ |= 1;
        notify_irq();
    }
}

// ── MMIO ──────────────────────────────────────────────────────────────────────

uint32_t Input::read(uint32_t off, uint32_t len) {
    std::lock_guard<std::mutex> lk(mtx_);

    if (off >= Reg::ConfigBase && off < Reg::ConfigBase + 256) {
        const uint32_t c_off     = off - Reg::ConfigBase;
        const uint32_t copy_len  = std::min(len, 256u - c_off);
        uint32_t val = 0;
        std::memcpy(&val, &config_[c_off], copy_len);
        return val;
    }

    switch (off) {
        case Reg::Magic:    return 0x74726976U;
        case Reg::Version:  return 2;
        case Reg::DeviceId: return kVirtioIdInput;
        case Reg::VendorId: return 0x554D4551U;
        case Reg::DeviceFeat:
            if (dev_feat_sel_ == 0) return static_cast<uint32_t>(device_features_);
            if (dev_feat_sel_ == 1) return static_cast<uint32_t>(device_features_ >> 32);
            return 0;
        case Reg::QueueNumMax: return kQueueSize;
        case Reg::QueueReady:  return queues_[queue_sel_].ready ? 1u : 0u;
        case Reg::IrqStatus:   return irq_status_;
        case Reg::Status:      return status_;
        default:               return 0;
    }
}

void Input::write(uint32_t off, uint32_t val, uint32_t len) {
    std::lock_guard<std::mutex> lk(mtx_);
    Queue& q = queues_[queue_sel_];

    if (off >= Reg::ConfigBase && off < Reg::ConfigBase + 256) {
        const uint32_t c_off    = off - Reg::ConfigBase;
        const uint32_t copy_len = std::min(len, 256u - c_off);
        std::memcpy(&config_[c_off], &val, copy_len);
        if (c_off == 0 || c_off == 1)
            update_config();
        return;
    }

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
        case Reg::QueueSel:   queue_sel_ = std::min(val, 1u); break;
        case Reg::QueueNum:   q.num      = std::min(val, kQueueSize); break;
        case Reg::QueueReady: q.ready    = (val != 0); break;
        case Reg::QueueNotify:
            if      (val == 0) flush_events_locked();
            else if (val == 1) process_statusq();
            break;
        case Reg::IrqAck: irq_status_ &= ~val; break;
        case Reg::Status:
            status_ = val;
            if (val == 0) {
                queues_[0] = queues_[1] = Queue{};
                irq_status_      = 0;
                driver_features_ = 0;
                pending_events_.clear();
            }
            break;
        case Reg::QueueDescLo:  q.desc_gpa  = (q.desc_gpa  & 0xFFFFFFFF00000000ULL) | val; break;
        case Reg::QueueDescHi:  q.desc_gpa  = (q.desc_gpa  & 0x00000000FFFFFFFFULL) | (static_cast<uint64_t>(val) << 32); break;
        case Reg::QueueAvailLo: q.avail_gpa = (q.avail_gpa & 0xFFFFFFFF00000000ULL) | val; break;
        case Reg::QueueAvailHi: q.avail_gpa = (q.avail_gpa & 0x00000000FFFFFFFFULL) | (static_cast<uint64_t>(val) << 32); break;
        case Reg::QueueUsedLo:  q.used_gpa  = (q.used_gpa  & 0xFFFFFFFF00000000ULL) | val; break;
        case Reg::QueueUsedHi:  q.used_gpa  = (q.used_gpa  & 0x00000000FFFFFFFFULL) | (static_cast<uint64_t>(val) << 32); break;
        default: break;
    }
}

} // namespace virtualization::virtio