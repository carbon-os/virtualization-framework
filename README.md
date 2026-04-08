# virtualization-framework

A cross-platform, hardware-accelerated virtualization framework for running arm64 Linux guests.
Provides a clean C++20 API over the native hypervisor on each platform —
Hypervisor.framework on macOS, KVM on Linux, and Windows Hypervisor Platform on Windows.

Available as `libvirtualization` via vcpkg.

---

## Features

- **Hardware acceleration** via HVF / KVM / WHP — no software emulation
- **virtio devices** — block, network, console, GPU (2D framebuffer + VirGL), keyboard, and tablet
- **GICv3 interrupt controller** emulation (arm64)
- **NAT networking** with host→guest port forwarding
- **arm64 Linux boot protocol** — loads kernel Image, initrd, and DTB directly; DTB is generated automatically from your configuration
- **Absolute pointer (tablet) device** — eliminates host/guest cursor drift entirely
- **Single header API** — `#include <virtualization/virtualization.hpp>` is all you need

---

## Requirements

| Platform | Compiler      | Hypervisor                                              | Min OS        |
|----------|---------------|---------------------------------------------------------|---------------|
| macOS    | Clang 17+     | Hypervisor.framework (Apple Silicon or Intel)           | macOS 11      |
| Linux    | GCC 12 / Clang 17 | KVM (`/dev/kvm` must be accessible)                | kernel 5.10+  |
| Windows  | MSVC 2022+    | Windows Hypervisor Platform (optional feature)          | Windows 11    |

- CMake 3.25+
- C++20

---

## Installation

### vcpkg (recommended)

```sh
vcpkg add port libvirtualization
```

Or add to your `vcpkg.json`:

```json
{
  "dependencies": [
    "libvirtualization"
  ]
}
```

Then in your `CMakeLists.txt`:

```cmake
find_package(libvirtualization CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE virtualization::virtualization)
```

### Build from source

```sh
git clone https://github.com/carbon-os/virtualization-framework
cd virtualization-framework
cmake -B build
cmake --build build
```

**macOS note:** sample binaries must be signed with the `com.apple.security.hypervisor`
entitlement before they will run. The build system does this automatically via `codesign`
if `resources/entitlements.plist` is present.

---

## Quick start

**Headless (console over stdin/stdout):**
```sh
build/samples/sample-vm ./data/vmlinuz ./data/debian-arm64.img ./data/initrd.img
```

**Graphical window with keyboard and mouse (macOS):**
```sh
build/samples/sample-vm-display-mac ./data/vmlinuz ./data/debian-arm64.img ./data/initrd.img
```

SSH is forwarded from host port `2222` to guest port `22`.

---

## API overview

### Minimal headless boot

```cpp
#include <virtualization/virtualization.hpp>
using namespace virtualization;

// Boot loader
LinuxBootLoader boot{"path/to/Image"};
boot.initrdURL   = "path/to/initrd";
boot.commandLine = "console=hvc0 root=/dev/vda rw quiet";

// Disk
DiskImageStorageDeviceAttachment disk{"path/to/disk.img", /*readOnly=*/false};
VirtioBlockDeviceConfiguration   block_dev{disk};

// Network with SSH forwarding
NATNetworkDeviceAttachment nat{};
nat.addPortForward({2222, 22});
VirtioNetworkDeviceConfiguration net_dev{nat};

// Console (stdin → guest, guest → stdout)
FileHandleSerialPortAttachment             console_attach{
    FileHandle::standardInput(),
    FileHandle::standardOutput()};
VirtioConsoleDeviceSerialPortConfiguration console_dev{console_attach};

// Assemble and run
VirtualMachineConfiguration config;
config.bootLoader = &boot;
config.cpuCount   = 2;
config.memorySize = 2 * GiB;
config.storageDevices.push_back(&block_dev);
config.networkDevices.push_back(&net_dev);
config.serialPorts.push_back(&console_dev);

config.validate(); // throws std::invalid_argument on misconfiguration

VirtualMachine vm{config};
vm.start([](std::error_code ec) {
    if (ec) fprintf(stderr, "VM stopped with error: %s\n", ec.message().c_str());
});
vm.waitUntilStopped();
```

### GPU + keyboard + tablet (graphical guest)

```cpp
// virtio-gpu 2D framebuffer — callback fires on every flushed frame
VirtioGPUDeviceConfiguration gpu_cfg{};
gpu_cfg.renderer = GPURenderer::Framebuffer2D;
gpu_cfg.width    = 1280;
gpu_cfg.height   = 800;
gpu_cfg.onFrameBufferUpdate = [](const uint8_t* pixels,
                                  uint32_t w, uint32_t h, uint32_t stride) {
    // blit pixels to your window / texture here
};

// Keyboard — inject Linux EV_KEY events via vm.sendKeyboardEvent()
VirtioInputDeviceConfiguration kbd_cfg;
kbd_cfg.type = InputDeviceType::Keyboard;

// Tablet — absolute pointer, eliminates cursor drift
VirtioInputDeviceConfiguration ptr_cfg;
ptr_cfg.type   = InputDeviceType::Tablet;
ptr_cfg.width  = 1280;
ptr_cfg.height = 800;

config.graphicsDevices.push_back(&gpu_cfg);
config.inputDevices.push_back(&kbd_cfg);
config.inputDevices.push_back(&ptr_cfg);

// After vm.start(), inject input events from your window system:
vm.sendKeyboardEvent(30, /*pressed=*/true);          // KEY_A down
vm.sendKeyboardEvent(30, /*pressed=*/false);         // KEY_A up
vm.sendPointerEvent(640, 400, /*left=*/true, /*right=*/false);
```

---

## Device support matrix

| Device                  | Class / Config                                    | Notes                                      |
|-------------------------|---------------------------------------------------|--------------------------------------------|
| Block (virtio-blk)      | `VirtioBlockDeviceConfiguration`                  | Raw disk image, read/write or read-only    |
| Network (virtio-net)    | `VirtioNetworkDeviceConfiguration`                | NAT with optional port forwarding          |
| Console (virtio-console)| `VirtioConsoleDeviceSerialPortConfiguration`      | File-handle backed, wires to stdin/stdout  |
| GPU 2D                  | `VirtioGPUDeviceConfiguration` (Framebuffer2D)    | Per-frame pixel callback                   |
| GPU 3D                  | `VirtioGPUDeviceConfiguration` (VirGL)            | VirGL accelerated OpenGL                   |
| Keyboard                | `VirtioInputDeviceConfiguration` (Keyboard)       | Full EV_KEY injection via `sendKeyboardEvent()` |
| Tablet / pointer        | `VirtioInputDeviceConfiguration` (Tablet)         | Absolute coords via `sendPointerEvent()`   |

---

## CMake options

| Option                    | Default | Description                            |
|---------------------------|---------|----------------------------------------|
| `VIRT_BUILD_SAMPLES`      | `ON`    | Build sample binaries                  |
| `VIRT_BUILD_SHARED`       | `OFF`   | Build as a shared library              |
| `VIRT_ENABLE_NETDEV`      | `ON`    | Build network device backends          |
| `VIRT_WARNINGS_AS_ERRORS` | `OFF`   | Treat compiler warnings as errors      |

---

## License

MIT