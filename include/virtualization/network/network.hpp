#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace virtualization::network {

struct PortForward { uint16_t host_port, guest_port; };

struct Config {
    // All IPs in host byte order (10.0.2.2 = 0x0A000202u)
    uint32_t gw_ip      = 0x0A000202u;  // 10.0.2.2  gateway
    uint32_t dns_ip     = 0x0A000203u;  // 10.0.2.3  virtual DNS
    uint32_t guest_ip   = 0x0A00020Fu;  // 10.0.2.15 DHCP offer
    uint32_t dns_fwd_ip = 0x08080808u;  // 8.8.8.8   upstream DNS

    uint8_t gw_mac   [6] = {0x52,0x55,0x0A,0x00,0x02,0x02};
    uint8_t guest_mac[6] = {0x52,0x54,0x00,0x12,0x34,0x56};

    std::vector<PortForward> port_forwards;
};

// Called by the stack to push a frame into the guest virtio-net RX ring.
// Must be thread-safe — called from multiple background threads.
using TxFn = std::function<void(const uint8_t* frame, size_t len)>;

class Stack {
public:
    explicit Stack(Config cfg, TxFn tx);
    ~Stack();
    Stack(const Stack&)            = delete;
    Stack& operator=(const Stack&) = delete;

    // Called by virtio-net when the guest transmits a frame.
    void guest_tx(const uint8_t* frame, size_t len);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace virtualization::network