#pragma once
#include "virtualization/vm_context.hpp"
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace virtualization::virtio {

inline constexpr uint16_t EV_SYN       = 0x00;
inline constexpr uint16_t EV_KEY       = 0x01;
inline constexpr uint16_t EV_ABS       = 0x03;

inline constexpr uint16_t SYN_REPORT   = 0x00;

inline constexpr uint16_t BTN_LEFT     = 0x110;
inline constexpr uint16_t BTN_RIGHT    = 0x111;
inline constexpr uint16_t BTN_MIDDLE   = 0x112;

inline constexpr uint16_t ABS_X        = 0x00;
inline constexpr uint16_t ABS_Y        = 0x01;

class Input {
public:
    enum class Type { Keyboard, Tablet };

    // spi_id is now passed explicitly — never derived from mmio_gpa
    Input(VMContext& ctx, uint64_t mmio_gpa, uint32_t spi_id, Type type,
          uint32_t abs_width = 0, uint32_t abs_height = 0) noexcept;
    ~Input() = default;

    Input(const Input&)            = delete;
    Input& operator=(const Input&) = delete;

    void send_key    (uint16_t linux_keycode, bool pressed);
    void send_pointer(uint32_t x, uint32_t y, bool btn_left, bool btn_right);

    [[nodiscard]] uint32_t read (uint32_t off, uint32_t len);
    void                   write(uint32_t off, uint32_t val, uint32_t len);
    void                   notify_irq();

private:
    static constexpr uint32_t kQueueSize = 64;

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

    struct virtio_input_event {
        uint16_t type;
        uint16_t code;
        uint32_t value;
    } __attribute__((packed));

    void update_config();
    void push_event_locked(uint16_t type, uint16_t code, uint32_t value);
    void flush_events_locked();
    void process_statusq();

    VMContext& ctx_;
    std::mutex mtx_;
    Type       type_;
    uint32_t   abs_width_;
    uint32_t   abs_height_;
    uint32_t   spi_id_;   // set directly from constructor arg, never computed

    uint32_t status_       {0};
    uint32_t queue_sel_    {0};
    uint32_t irq_status_   {0};
    uint32_t dev_feat_sel_ {0};
    uint32_t drv_feat_sel_ {0};

    uint64_t device_features_;
    uint64_t driver_features_ {0};

    Queue queues_[2];

    std::deque<virtio_input_event> pending_events_;
    uint8_t config_[256] {0};
};

} // namespace virtualization::virtio