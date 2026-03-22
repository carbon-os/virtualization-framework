# virtualization_framework

> A cross-platform virtualization framework by **carbonOS**  
> Stable, simple, and reusable — built for applications and embedded systems alike.

---

## Overview

`virtualization::` is a modern C++20 framework developed by **carbonOS** for creating
and managing virtual machines across any platform. Inspired by Apple's Virtualization.framework,
it brings the same clean, expressive API design to Linux, Windows, embedded targets, and beyond.

Whether you are building a desktop hypervisor, a sandboxed application runtime, an embedded
system emulator, or a custom render pipeline — `virtualization::` gives you a single stable
API that stays out of your way.

---

## Goals

- **Cross-platform** — runs on Linux, Windows, macOS, and embedded targets
- **Stable API** — no churn, no surprises between versions
- **Simple by default** — sane defaults, fluent configuration, minimal boilerplate
- **Reusable** — plugs into any renderer, input system, or audio backend
- **Embeddable** — designed to live inside larger applications, not just standalone hypervisors

---

## Requirements

| | Minimum |
|---|---|
| C++ Standard | C++20 |
| CMake | 3.24+ |
| Compiler | GCC 12+, Clang 14+, MSVC 19.34+ |

---

## Installation
```cmake
include(FetchContent)

FetchContent_Declare(
    virtualization
    GIT_REPOSITORY https://github.com/carbonOS/virtualization-framework
    GIT_TAG        stable
)

FetchContent_MakeAvailable(virtualization)
target_link_libraries(your_app PRIVATE virtualization::virtualization)
```

---

## Quick Start
```cpp
#include <virtualization/virtualization.hpp>
using namespace virtualization;

int main() {
    Error error;

    VirtualMachineConfiguration config;
    config.setCpuCount(4)
          .setMemorySize(4ULL * 1024 * 1024 * 1024)
          .setBootLoader(std::make_shared<LinuxBootLoader>(
              "/boot/vmlinuz", "/boot/initrd.img", "root=/dev/vda1"))
          .setStorageDevices({
              std::make_shared<VirtioBlockDevice>(
                  std::make_shared<DiskImageAttachment>("/vms/disk.img", false))
          })
          .setNetworkDevices({
              std::make_shared<VirtioNetworkDevice>(
                  std::make_shared<NatAttachment>())
          });

    if (!config.validate(error)) {
        std::cerr << "Configuration error: " << error.message << "\n";
        return 1;
    }

    VirtualMachine vm(config, Executor::makeBackground("vm.queue"));

    vm.start([](std::optional<Error> err) {
        if (!err) std::cout << "VM is running\n";
    });

    return 0;
}
```

---

## Configuration

### CPU & Memory
```cpp
config.setCpuCount(4)
      .setMemorySize(4ULL * 1024 * 1024 * 1024);  // 4 GB
```

### Boot Loaders
```cpp
// Linux
config.setBootLoader(std::make_shared<LinuxBootLoader>(
    "/boot/vmlinuz", "/boot/initrd.img", "root=/dev/vda1 quiet"));

// EFI
config.setBootLoader(std::make_shared<EFIBootLoader>(
    "/firmware/OVMF.fd"));
```

### Storage
```cpp
config.setStorageDevices({
    std::make_shared<VirtioBlockDevice>(
        std::make_shared<DiskImageAttachment>("/vms/disk.img", /*readOnly=*/false))
});
```

### Networking
```cpp
// NAT — guest accesses internet through host
auto nat = std::make_shared<VirtioNetworkDevice>(
               std::make_shared<NatAttachment>());

// Bridged — guest shares a real host interface
auto iface  = BridgedInterface::networkInterfaces().front();
auto bridge = std::make_shared<VirtioNetworkDevice>(
                  std::make_shared<BridgedAttachment>(iface));

config.setNetworkDevices({ nat });
```

### Graphics
```cpp
auto gpu = std::make_shared<VirtioGraphicsDevice>();
gpu->setScanouts({ std::make_shared<GraphicsScanout>(1920, 1080) });

config.setGraphicsDevices({ gpu });
```

### Input
```cpp
config.setKeyboards({ std::make_shared<USBKeyboard>() })
      .setPointingDevices({ std::make_shared<USBPointerDevice>() });
```

### Audio
```cpp
config.setAudioDevices({ std::make_shared<VirtioAudioDevice>() });
```

### Serial
```cpp
config.setSerialDevices({ std::make_shared<VirtioSerialDevice>() });
```

### Validation
```cpp
Error error;
if (!config.validate(error)) {
    std::cerr << error.message << "\n";
}
```

---

## Virtual Machine Lifecycle
```cpp
VirtualMachine vm(config, Executor::makeBackground("vm.queue"));

// Start
vm.start([](std::optional<Error> err) { ... });

// Pause / Resume
vm.pause([](std::optional<Error> err) { ... });
vm.resume([](std::optional<Error> err) { ... });

// Stop
vm.stop([](std::optional<Error> err) { ... });

// Snapshot
vm.saveState("/snapshots/vm0.state",
    [](std::optional<Error> err) { ... });

vm.restoreState("/snapshots/vm0.state",
    [](std::optional<Error> err) { ... });

// State inspection
MachineState state = vm.state();
bool canStart      = vm.canStart();
bool canPause      = vm.canPause();
bool canResume     = vm.canResume();
bool canStop       = vm.canStop();
```

### Machine States

| State | Description |
|---|---|
| `Stopped` | VM is not running |
| `Starting` | Boot sequence in progress |
| `Running` | Guest is active |
| `Pausing` | Transitioning to paused |
| `Paused` | Execution suspended |
| `Resuming` | Transitioning back to running |
| `Stopping` | Shutdown in progress |
| `Error` | VM encountered a fatal error |

---

## Delegate
```cpp
class MyDelegate : public VirtualMachineDelegate {
public:
    void guestDidStop(VirtualMachine& vm) override {
        std::cout << "Guest stopped cleanly\n";
    }

    void virtualMachineDidStop(VirtualMachine& vm,
                               const Error& error) override {
        std::cerr << "VM crashed: " << error.message << "\n";
    }

    void networkDeviceWasDisconnected(VirtualMachine& vm,
                                      const NetworkDevice& device,
                                      const Error& error) override {
        std::cerr << "NIC lost: " << device.identifier << "\n";
    }
};

vm.setDelegate(&myDelegate);
```

---

## I/O Handlers

Handlers are the bridge between the VM and your application.
Attach them to devices before starting the VM.

### Display
```cpp
gpu->setFrameHandler([](const FrameBuffer& fb) {
    // fb.pixels  — raw RGBA8 bytes
    // fb.width   — frame width
    // fb.height  — frame height
    // fb.stride  — bytes per row
    // fb.timestampNs — guest clock timestamp
});

gpu->setResolutionHandler([](uint32_t w, uint32_t h) {
    // guest requested a resolution change
});
```

### Keyboard
```cpp
// Send a key event into the guest
keyboard->sendEvent({
    .scanCode  = 30,
    .keySym    = SDLK_a,
    .modifiers = 0,
    .action    = KeyEvent::Action::Press
});
```

### Pointer
```cpp
// Send a pointer event into the guest
pointer->sendEvent({
    .x      = 0.5f,           // normalized 0.0 – 1.0
    .y      = 0.5f,
    .button = PointerEvent::Button::Left,
    .action = PointerEvent::Action::Press
});
```

### Audio
```cpp
// Guest audio output → your speaker / backend
audio->setOutputHandler([](const AudioBuffer& buf) {
    // buf.samples    — interleaved float32 PCM
    // buf.frameCount — number of frames
    // buf.channels   — channel count
    // buf.sampleRate — sample rate in Hz
});

// Your microphone / source → guest audio input
audio->setInputHandler([](float* outSamples,
                           uint32_t frameCount,
                           uint8_t  channels) {
    // fill outSamples from your capture device
});
```

### Serial / Console
```cpp
// Guest writes → your handler
serial->setReadHandler([](std::span<const uint8_t> data) {
    std::cout.write((const char*)data.data(), data.size());
});

// You write → guest
serial->write(std::span<const uint8_t>((const uint8_t*)"ls -la\n", 7));
```

### Network Packets
```cpp
// Guest sends a packet → your handler
net->setPacketHandler([](std::span<const uint8_t> frame) {
    write(tunFd, frame.data(), frame.size());
});

// You send a packet → guest
net->sendPacket(std::span<const uint8_t>(incoming.data(), n));
```

---

## Renderer Integration Examples

### SDL2
```cpp
gpu->setFrameHandler([&](const FrameBuffer& fb) {
    SDL_UpdateTexture(texture, nullptr, fb.pixels, fb.stride);
    SDL_RenderCopy(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
});

gpu->setResolutionHandler([&](uint32_t w, uint32_t h) {
    SDL_SetWindowSize(window, w, h);
});

// SDL2 event loop
SDL_Event e;
while (running) {
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) { vm.stop({}); running = false; }

        if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP)
            keyboard->sendEvent({
                .scanCode = (uint16_t)e.key.keysym.scancode,
                .keySym   = (uint32_t)e.key.keysym.sym,
                .action   = e.type == SDL_KEYDOWN
                              ? KeyEvent::Action::Press
                              : KeyEvent::Action::Release
            });

        if (e.type == SDL_MOUSEMOTION)
            pointer->sendEvent({
                .x      = (float)e.motion.x / windowWidth,
                .y      = (float)e.motion.y / windowHeight,
                .action = PointerEvent::Action::Move
            });
    }
}
```

### Qt
```cpp
gpu->setFrameHandler([&](const FrameBuffer& fb) {
    QImage image(fb.pixels, fb.width, fb.height,
                 fb.stride, QImage::Format_RGBA8888);
    label->setPixmap(QPixmap::fromImage(image));
});

gpu->setResolutionHandler([&](uint32_t w, uint32_t h) {
    window->resize(w, h);
});

void MyWidget::keyPressEvent(QKeyEvent* e) {
    keyboard->sendEvent({
        .keySym   = (uint32_t)e->key(),
        .scanCode = (uint16_t)e->nativeScanCode(),
        .action   = KeyEvent::Action::Press
    });
}

void MyWidget::mouseMoveEvent(QMouseEvent* e) {
    pointer->sendEvent({
        .x      = (float)e->pos().x() / width(),
        .y      = (float)e->pos().y() / height(),
        .action = PointerEvent::Action::Move
    });
}
```

### Objective-C / Metal
```objc
gpu->setFrameHandler([&](const virtualization::FrameBuffer& fb) {
    MTLTextureDescriptor *desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                          width:fb.width
                                                         height:fb.height
                                                      mipmapped:NO];

    id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
    [texture replaceRegion:MTLRegionMake2D(0, 0, fb.width, fb.height)
               mipmapLevel:0
                 withBytes:fb.pixels
               bytesPerRow:fb.stride];

    // pass texture to your Metal render pipeline
    [renderLayer setTexture:texture];
});
```

---

## Executor

The executor controls which thread VM events and callbacks are delivered on.
```cpp
// Background thread pool — recommended for VM work
auto exec = Executor::makeBackground("vm.queue");

// Main / UI thread — use when callbacks touch UI directly
auto exec = Executor::makeMainThread();

// Serial queue — ordered, single-threaded delivery
auto exec = Executor::makeSerial("vm.serial");
```

---

## Embedded Systems

`virtualization::` is designed to be lightweight enough for embedded targets.
Unused subsystems (audio, graphics, serial) can be excluded at compile time.
```cmake
target_compile_definitions(your_app PRIVATE
    VZ_NO_AUDIO
    VZ_NO_GRAPHICS
)
```

Minimal embedded configuration:
```cpp
VirtualMachineConfiguration config;
config.setCpuCount(1)
      .setMemorySize(256ULL * 1024 * 1024)    // 256 MB
      .setBootLoader(std::make_shared<LinuxBootLoader>(
          "/boot/vmlinuz", "/boot/initrd.img", "console=ttyS0"))
      .setSerialDevices({ std::make_shared<VirtioSerialDevice>() });

serial->setReadHandler([](std::span<const uint8_t> data) {
    // handle guest console output
});
```

---

## License

`virtualization::` is developed and maintained by **carbonOS**.  
See `LICENSE` for terms.

---

> **carbonOS** — building the foundation for the next generation of operating systems.
