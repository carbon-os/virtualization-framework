#pragma once
// Public umbrella header — the only header end-users need to include.
//
//   #include <virtualization/virtualization.hpp>
//   using namespace virtualization;

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace virtualization {

// ── Size helpers ──────────────────────────────────────────────────
inline constexpr uint64_t GiB = 1024ULL * 1024ULL * 1024ULL;
inline constexpr uint64_t MiB = 1024ULL * 1024ULL;

// ─────────────────────────────────────────────────────────────────
// FileHandle
// ─────────────────────────────────────────────────────────────────

class FileHandle {
public:
    explicit FileHandle(int fd) noexcept : fd_(fd) {}

    [[nodiscard]] static FileHandle standardInput()  noexcept;
    [[nodiscard]] static FileHandle standardOutput() noexcept;

    [[nodiscard]] int fd() const noexcept { return fd_; }

private:
    int fd_;
};

// ─────────────────────────────────────────────────────────────────
// MACAddress
// ─────────────────────────────────────────────────────────────────

class MACAddress {
public:
    /// Parse a colon-separated hex string: "52:54:00:12:34:56".
    /// @throws std::invalid_argument on malformed input.
    explicit MACAddress(std::string_view str);

    MACAddress(uint8_t b0, uint8_t b1, uint8_t b2,
               uint8_t b3, uint8_t b4, uint8_t b5) noexcept;

    [[nodiscard]] const uint8_t* bytes() const noexcept { return bytes_; }

private:
    uint8_t bytes_[6] {};
};

// ─────────────────────────────────────────────────────────────────
// PortForward
// ─────────────────────────────────────────────────────────────────

struct PortForward {
    PortForward(uint16_t hostPort, uint16_t guestPort) noexcept
        : hostPort(hostPort), guestPort(guestPort) {}

    uint16_t hostPort;
    uint16_t guestPort;
};

// ═════════════════════════════════════════════════════════════════
// Boot loaders
// ═════════════════════════════════════════════════════════════════

struct BootLoader {
    virtual ~BootLoader() = default;
};

/// Configuration for the standard arm64 Linux boot protocol.
struct LinuxBootLoader : BootLoader {
    /// @param kernelURL  Path to the uncompressed arm64 kernel Image.
    explicit LinuxBootLoader(std::string kernelURL)
        : kernelURL(std::move(kernelURL)) {}

    std::string kernelURL;   ///< Required.
    std::string initrdURL;   ///< Optional initrd / initramfs path.
    std::string commandLine; ///< Kernel command-line string.
};

// ═════════════════════════════════════════════════════════════════
// Storage devices
// ═════════════════════════════════════════════════════════════════

struct StorageDeviceAttachment {
    virtual ~StorageDeviceAttachment() = default;
};

/// Attach a raw disk image file as a virtio-blk device.
struct DiskImageStorageDeviceAttachment : StorageDeviceAttachment {
    DiskImageStorageDeviceAttachment(std::string imageURL, bool readOnly) noexcept
        : imageURL(std::move(imageURL)), readOnly(readOnly) {}

    std::string imageURL;
    bool        readOnly;
};

struct VirtioBlockDeviceConfiguration {
    explicit VirtioBlockDeviceConfiguration(StorageDeviceAttachment& attachment)
        : attachment(&attachment) {}

    StorageDeviceAttachment* attachment {nullptr};
};

// ═════════════════════════════════════════════════════════════════
// Network devices
// ═════════════════════════════════════════════════════════════════

struct NetworkDeviceAttachment {
    virtual ~NetworkDeviceAttachment() = default;
};

/// Userspace NAT stack with optional host→guest port forwarding.
struct NATNetworkDeviceAttachment : NetworkDeviceAttachment {
    void addPortForward(PortForward pf) { portForwards.push_back(pf); }

    std::vector<PortForward> portForwards;
};

struct VirtioNetworkDeviceConfiguration {
    explicit VirtioNetworkDeviceConfiguration(NetworkDeviceAttachment& attachment)
        : attachment(&attachment) {}

    NetworkDeviceAttachment* attachment {nullptr};
    MACAddress               macAddress {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
};

// ═════════════════════════════════════════════════════════════════
// Serial ports / Console
// ═════════════════════════════════════════════════════════════════

struct SerialPortAttachment {
    virtual ~SerialPortAttachment() = default;
};

/// Route guest console I/O through a pair of file descriptors.
struct FileHandleSerialPortAttachment : SerialPortAttachment {
    FileHandleSerialPortAttachment(FileHandle input, FileHandle output) noexcept
        : input(input), output(output) {}

    FileHandle input;
    FileHandle output;
};

struct VirtioConsoleDeviceSerialPortConfiguration {
    explicit VirtioConsoleDeviceSerialPortConfiguration(SerialPortAttachment& attachment)
        : attachment(&attachment) {}

    SerialPortAttachment* attachment {nullptr};
};

// ═════════════════════════════════════════════════════════════════
// GPU
// ═════════════════════════════════════════════════════════════════

enum class GPURenderer {
    Framebuffer2D,
    VirGL,
};

struct VirtioGPUDeviceConfiguration {
    GPURenderer renderer {GPURenderer::Framebuffer2D};

    uint32_t width  {1280}; ///< Display width  in pixels.
    uint32_t height {800};  ///< Display height in pixels.

    std::function<void(const uint8_t* pixels,
                       uint32_t       width,
                       uint32_t       height,
                       uint32_t       stride)> onFrameBufferUpdate;
};

// ═════════════════════════════════════════════════════════════════
// Input Devices
// ═════════════════════════════════════════════════════════════════

enum class InputDeviceType {
    Keyboard,
    Tablet ///< Absolute pointer device (prevents host/guest cursor drift)
};

struct VirtioInputDeviceConfiguration {
    InputDeviceType type {InputDeviceType::Keyboard};

    /// Only used if type == Tablet to properly scale coordinates.
    uint32_t width  {1280};
    uint32_t height {800};
};

// ═════════════════════════════════════════════════════════════════
// VirtualMachineConfiguration
// ═════════════════════════════════════════════════════════════════

class VirtualMachineConfiguration {
public:
    BootLoader* bootLoader  {nullptr};
    uint32_t     cpuCount    {1};
    uint64_t     memorySize  {512 * MiB};

    std::vector<VirtioBlockDeviceConfiguration*>             storageDevices;
    std::vector<VirtioNetworkDeviceConfiguration*>           networkDevices;
    std::vector<VirtioConsoleDeviceSerialPortConfiguration*> serialPorts;
    std::vector<VirtioGPUDeviceConfiguration*>               graphicsDevices;
    std::vector<VirtioInputDeviceConfiguration*>             inputDevices;

    /// Validates the configuration.
    /// @throws std::invalid_argument describing the first violated constraint.
    void validate() const;
};

// ═════════════════════════════════════════════════════════════════
// VirtualMachine
// ═════════════════════════════════════════════════════════════════

class VirtualMachine {
public:
    /// Builds internal state from config but does not start any threads.
    /// @throws std::runtime_error if resource allocation fails.
    explicit VirtualMachine(const VirtualMachineConfiguration& config);

    ~VirtualMachine();

    VirtualMachine(const VirtualMachine&)            = delete;
    VirtualMachine& operator=(const VirtualMachine&) = delete;
    VirtualMachine(VirtualMachine&&)                 = delete;
    VirtualMachine& operator=(VirtualMachine&&)      = delete;

    /// Starts the VM.  Returns immediately; the VM runs on background threads.
    void start(std::function<void(std::error_code)> completionHandler = {});

    /// Requests an immediate stop (equivalent to asserting PSCI SYSTEM_OFF).
    void stop();

    /// Blocks the calling thread until the VM has fully stopped.
    void waitUntilStopped();

    // ── Input Injection ───────────────────────────────────────────────────────

    /// Injects a keyboard event into the guest.
    /// @param linux_keycode Standard Linux EV_KEY code (e.g., KEY_A = 30)
    /// @param pressed true for keydown, false for keyup
    void sendKeyboardEvent(uint16_t linux_keycode, bool pressed);

    /// Injects an absolute pointer (tablet) event into the guest.
    /// @param x absolute X coordinate [0, display_width]
    /// @param y absolute Y coordinate [0, display_height]
    void sendPointerEvent(uint32_t x, uint32_t y, bool btn_left, bool btn_right);

private:
    struct Impl;                    // defined in src/core/vm.cpp
    std::unique_ptr<Impl> impl_;
};

} // namespace virtualization