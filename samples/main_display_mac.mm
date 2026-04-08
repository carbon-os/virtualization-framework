// samples/main_display_mac.mm
//
// macOS sample: boot a Linux arm64 guest with a virtio-gpu 2D framebuffer,
// virtio-input keyboard and tablet, and paint every flushed frame into a
// native NSWindow via CoreGraphics / CALayer.
//
// Usage: sample-vm-display-mac <kernel-Image> <disk.img> [initrd]

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include <virtualization/virtualization.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <system_error>
#include <vector>

// ── Guest framebuffer dimensions ───────────────────────────────────────────────

static constexpr uint32_t kGuestWidth  = 1280;
static constexpr uint32_t kGuestHeight = 800;

// ── Global VM pointer (window-close handler) ──────────────────────────────────

static virtualization::VirtualMachine* g_vm = nullptr;

// ── macOS → Linux keycode translation table ───────────────────────────────────
//
// Indexed directly by NSEvent.keyCode (macOS virtual keycode, 0x00–0x7F).
// macOS keycodes follow physical key position on a US ANSI keyboard, not ASCII.
// Linux codes are standard EV_KEY values from <linux/input-event-codes.h>.
//
// 0 means "no mapping / ignore".

static uint16_t mac_to_linux_keycode(uint16_t mac) noexcept {
    // clang-format off
    static constexpr uint16_t kTable[128] = {
        /* 0x00 kVK_ANSI_A            */ 30,  // KEY_A
        /* 0x01 kVK_ANSI_S            */ 31,  // KEY_S
        /* 0x02 kVK_ANSI_D            */ 32,  // KEY_D
        /* 0x03 kVK_ANSI_F            */ 33,  // KEY_F
        /* 0x04 kVK_ANSI_H            */ 35,  // KEY_H
        /* 0x05 kVK_ANSI_G            */ 34,  // KEY_G
        /* 0x06 kVK_ANSI_Z            */ 44,  // KEY_Z
        /* 0x07 kVK_ANSI_X            */ 45,  // KEY_X
        /* 0x08 kVK_ANSI_C            */ 46,  // KEY_C
        /* 0x09 kVK_ANSI_V            */ 47,  // KEY_V
        /* 0x0A ISO extra key (§/±)   */  0,
        /* 0x0B kVK_ANSI_B            */ 48,  // KEY_B
        /* 0x0C kVK_ANSI_Q            */ 16,  // KEY_Q
        /* 0x0D kVK_ANSI_W            */ 17,  // KEY_W
        /* 0x0E kVK_ANSI_E            */ 18,  // KEY_E
        /* 0x0F kVK_ANSI_R            */ 19,  // KEY_R
        /* 0x10 kVK_ANSI_Y            */ 21,  // KEY_Y
        /* 0x11 kVK_ANSI_T            */ 20,  // KEY_T
        /* 0x12 kVK_ANSI_1            */  2,  // KEY_1
        /* 0x13 kVK_ANSI_2            */  3,  // KEY_2
        /* 0x14 kVK_ANSI_3            */  4,  // KEY_3
        /* 0x15 kVK_ANSI_4            */  5,  // KEY_4
        /* 0x16 kVK_ANSI_6            */  7,  // KEY_6
        /* 0x17 kVK_ANSI_5            */  6,  // KEY_5
        /* 0x18 kVK_ANSI_Equal        */ 13,  // KEY_EQUAL
        /* 0x19 kVK_ANSI_9            */ 10,  // KEY_9
        /* 0x1A kVK_ANSI_7            */  8,  // KEY_7
        /* 0x1B kVK_ANSI_Minus        */ 12,  // KEY_MINUS
        /* 0x1C kVK_ANSI_8            */  9,  // KEY_8
        /* 0x1D kVK_ANSI_0            */ 11,  // KEY_0
        /* 0x1E kVK_ANSI_RightBracket */ 27,  // KEY_RIGHTBRACE
        /* 0x1F kVK_ANSI_O            */ 24,  // KEY_O
        /* 0x20 kVK_ANSI_U            */ 22,  // KEY_U
        /* 0x21 kVK_ANSI_LeftBracket  */ 26,  // KEY_LEFTBRACE
        /* 0x22 kVK_ANSI_I            */ 23,  // KEY_I
        /* 0x23 kVK_ANSI_P            */ 25,  // KEY_P
        /* 0x24 kVK_Return            */ 28,  // KEY_ENTER
        /* 0x25 kVK_ANSI_L            */ 38,  // KEY_L
        /* 0x26 kVK_ANSI_J            */ 36,  // KEY_J
        /* 0x27 kVK_ANSI_Quote        */ 40,  // KEY_APOSTROPHE
        /* 0x28 kVK_ANSI_K            */ 37,  // KEY_K
        /* 0x29 kVK_ANSI_Semicolon    */ 39,  // KEY_SEMICOLON
        /* 0x2A kVK_ANSI_Backslash    */ 43,  // KEY_BACKSLASH
        /* 0x2B kVK_ANSI_Comma        */ 51,  // KEY_COMMA
        /* 0x2C kVK_ANSI_Slash        */ 53,  // KEY_SLASH
        /* 0x2D kVK_ANSI_N            */ 49,  // KEY_N
        /* 0x2E kVK_ANSI_M            */ 50,  // KEY_M
        /* 0x2F kVK_ANSI_Period       */ 52,  // KEY_DOT
        /* 0x30 kVK_Tab               */ 15,  // KEY_TAB
        /* 0x31 kVK_Space             */ 57,  // KEY_SPACE
        /* 0x32 kVK_ANSI_Grave        */ 41,  // KEY_GRAVE
        /* 0x33 kVK_Delete            */ 14,  // KEY_BACKSPACE
        /* 0x34 (unused)              */  0,
        /* 0x35 kVK_Escape            */  1,  // KEY_ESC
        /* 0x36 kVK_RightCommand      */ 126, // KEY_RIGHTMETA
        /* 0x37 kVK_Command           */ 125, // KEY_LEFTMETA
        /* 0x38 kVK_Shift             */ 42,  // KEY_LEFTSHIFT
        /* 0x39 kVK_CapsLock          */ 58,  // KEY_CAPSLOCK
        /* 0x3A kVK_Option            */ 56,  // KEY_LEFTALT
        /* 0x3B kVK_Control           */ 29,  // KEY_LEFTCTRL
        /* 0x3C kVK_RightShift        */ 54,  // KEY_RIGHTSHIFT
        /* 0x3D kVK_RightOption       */ 100, // KEY_RIGHTALT
        /* 0x3E kVK_RightControl      */ 97,  // KEY_RIGHTCTRL
        /* 0x3F kVK_Function          */  0,
        /* 0x40 kVK_F17               */ 183, // KEY_F17
        /* 0x41 kVK_ANSI_KeypadDecimal*/ 83,  // KEY_KPDOT
        /* 0x42 (unused)              */  0,
        /* 0x43 kVK_ANSI_KeypadMultiply*/55,  // KEY_KPASTERISK
        /* 0x44 (unused)              */  0,
        /* 0x45 kVK_ANSI_KeypadPlus   */ 78,  // KEY_KPPLUS
        /* 0x46 (unused)              */  0,
        /* 0x47 kVK_ANSI_KeypadClear  */ 69,  // KEY_NUMLOCK
        /* 0x48 (unused)              */  0,
        /* 0x49 (unused)              */  0,
        /* 0x4A (unused)              */  0,
        /* 0x4B kVK_ANSI_KeypadDivide */ 98,  // KEY_KPSLASH
        /* 0x4C kVK_ANSI_KeypadEnter  */ 96,  // KEY_KPENTER
        /* 0x4D (unused)              */  0,
        /* 0x4E kVK_ANSI_KeypadMinus  */ 74,  // KEY_KPMINUS
        /* 0x4F kVK_F18               */ 184, // KEY_F18
        /* 0x50 kVK_F19               */ 185, // KEY_F19
        /* 0x51 kVK_ANSI_KeypadEquals */ 117, // KEY_KPEQUAL
        /* 0x52 kVK_ANSI_Keypad0      */ 82,  // KEY_KP0
        /* 0x53 kVK_ANSI_Keypad1      */ 79,  // KEY_KP1
        /* 0x54 kVK_ANSI_Keypad2      */ 80,  // KEY_KP2
        /* 0x55 kVK_ANSI_Keypad3      */ 81,  // KEY_KP3
        /* 0x56 kVK_ANSI_Keypad4      */ 75,  // KEY_KP4
        /* 0x57 kVK_ANSI_Keypad5      */ 76,  // KEY_KP5
        /* 0x58 kVK_ANSI_Keypad6      */ 77,  // KEY_KP6
        /* 0x59 kVK_ANSI_Keypad7      */ 71,  // KEY_KP7
        /* 0x5A kVK_F20               */ 186, // KEY_F20
        /* 0x5B kVK_ANSI_Keypad8      */ 72,  // KEY_KP8
        /* 0x5C kVK_ANSI_Keypad9      */ 73,  // KEY_KP9
        /* 0x5D (unused)              */  0,
        /* 0x5E (unused)              */  0,
        /* 0x5F (unused)              */  0,
        /* 0x60 kVK_F5                */ 63,  // KEY_F5
        /* 0x61 kVK_F6                */ 64,  // KEY_F6
        /* 0x62 kVK_F7                */ 65,  // KEY_F7
        /* 0x63 kVK_F3                */ 61,  // KEY_F3
        /* 0x64 kVK_F8                */ 66,  // KEY_F8
        /* 0x65 kVK_F9                */ 67,  // KEY_F9
        /* 0x66 (unused)              */  0,
        /* 0x67 kVK_F11               */ 87,  // KEY_F11
        /* 0x68 (unused)              */  0,
        /* 0x69 kVK_F13 / PrintScreen */ 99,  // KEY_SYSRQ
        /* 0x6A kVK_F16               */ 186, // KEY_F16 (no standard, reuse)
        /* 0x6B kVK_F14 / ScrollLock  */ 70,  // KEY_SCROLLLOCK
        /* 0x6C (unused)              */  0,
        /* 0x6D kVK_F10               */ 68,  // KEY_F10
        /* 0x6E (unused)              */  0,
        /* 0x6F kVK_F12               */ 88,  // KEY_F12
        /* 0x70 (unused)              */  0,
        /* 0x71 kVK_F15 / Pause       */ 119, // KEY_PAUSE
        /* 0x72 kVK_Help / Insert     */ 110, // KEY_INSERT
        /* 0x73 kVK_Home              */ 102, // KEY_HOME
        /* 0x74 kVK_PageUp            */ 104, // KEY_PAGEUP
        /* 0x75 kVK_ForwardDelete     */ 111, // KEY_DELETE
        /* 0x76 kVK_F4                */ 62,  // KEY_F4
        /* 0x77 kVK_End               */ 107, // KEY_END
        /* 0x78 kVK_F2                */ 60,  // KEY_F2
        /* 0x79 kVK_PageDown          */ 109, // KEY_PAGEDOWN
        /* 0x7A kVK_F1                */ 59,  // KEY_F1
        /* 0x7B kVK_LeftArrow         */ 105, // KEY_LEFT
        /* 0x7C kVK_RightArrow        */ 106, // KEY_RIGHT
        /* 0x7D kVK_DownArrow         */ 108, // KEY_DOWN
        /* 0x7E kVK_UpArrow           */ 103, // KEY_UP
        /* 0x7F (unused)              */  0,
    };
    // clang-format on
    return (mac < 128) ? kTable[mac] : 0;
}

// ── FrameView ──────────────────────────────────────────────────────────────────

@interface FrameView : NSView
- (void)updateWithPixels:(const uint8_t*)pixels
                   width:(uint32_t)w
                  height:(uint32_t)h
                  stride:(uint32_t)stride;
@end

@implementation FrameView {
    CGImageRef       _image;
    NSTrackingArea  *_trackingArea;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.wantsLayer            = YES;
        self.layer.backgroundColor = NSColor.blackColor.CGColor;
        // kCAGravityResize: image always fills the full layer with no letterboxing.
        // This is required for correct mouse coordinate mapping — kCAGravityResizeAspect
        // would add letterbox margins on any non-exact resize, shifting all coordinates.
        self.layer.contentsGravity = kCAGravityResize;
        _image = nil;
        [self _setupTrackingArea];
    }
    return self;
}

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)isFlipped              { return NO;  }

- (void)_setupTrackingArea {
    _trackingArea = [[NSTrackingArea alloc]
        initWithRect:self.bounds
             options:(  NSTrackingMouseMoved
                      | NSTrackingMouseEnteredAndExited
                      | NSTrackingActiveAlways
                      | NSTrackingInVisibleRect)
               owner:self
            userInfo:nil];
    [self addTrackingArea:_trackingArea];
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    [self removeTrackingArea:_trackingArea];
    [self _setupTrackingArea];
}

// ── Rendering ─────────────────────────────────────────────────────────────────

- (void)updateWithPixels:(const uint8_t*)pixels
                   width:(uint32_t)w
                  height:(uint32_t)h
                  stride:(uint32_t)stride
{
    NSData           *data = [NSData dataWithBytes:pixels
                                            length:(NSUInteger)stride * h];
    CGColorSpaceRef   cs   = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp   = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);

    CGImageRef img = CGImageCreate(
        w, h,
        /*bitsPerComponent=*/8, /*bitsPerPixel=*/32, /*bytesPerRow=*/stride,
        cs,
        (CGBitmapInfo)(kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst),
        dp, nullptr, false, kCGRenderingIntentDefault);

    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);

    if (_image) CGImageRelease(_image);
    _image = img;

    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    self.layer.contents = (__bridge id)img;
    [CATransaction commit];
}

// ── Keyboard input ────────────────────────────────────────────────────────────

- (void)keyDown:(NSEvent *)event {
    if (!g_vm) return;
    const uint16_t linux_key = mac_to_linux_keycode(event.keyCode);
    if (linux_key != 0)
        g_vm->sendKeyboardEvent(linux_key, /*pressed=*/true);
}

- (void)keyUp:(NSEvent *)event {
    if (!g_vm) return;
    const uint16_t linux_key = mac_to_linux_keycode(event.keyCode);
    if (linux_key != 0)
        g_vm->sendKeyboardEvent(linux_key, /*pressed=*/false);
}

- (BOOL)performKeyEquivalent:(NSEvent *)__unused event { return YES; }

// ── Pointer input ─────────────────────────────────────────────────────────────
//
// Coordinate pipeline:
//   1. locationInWindow  → window base coords (points, origin bottom-left of content)
//   2. convertPoint:fromView:nil → view-local points (same origin, accounts for any
//      view hierarchy transforms)
//   3. convertPointToBacking: → physical backing pixels (Retina-aware; on a 2× display
//      this doubles the values so we're always in real screen pixels)
//   4. Scale from backing pixels to guest pixels using the backing pixel size of the
//      view — not the point size — so the mapping is correct at any DPI and any
//      window size.
//   5. Flip Y: macOS origin is bottom-left; guest expects top-left.
//
// Because we use kCAGravityResize the framebuffer always fills the entire view with
// no letterboxing, so the linear scale is always exact.

- (void)_handlePointerEvent:(NSEvent *)event {
    if (!g_vm) return;

    // Step 1-2: view-local point coordinates.
    const NSPoint ptView = [self convertPoint:event.locationInWindow fromView:nil];

    // Step 3: convert to backing (physical) pixels — handles Retina scaling.
    const NSPoint ptPx   = [self convertPointToBacking:ptView];
    const NSSize  szPx   = [self convertSizeToBacking:self.bounds.size];

    if (szPx.width <= 0 || szPx.height <= 0) return;

    // Step 4-5: map to guest pixel space, flip Y.
    const double scaleX = (double)kGuestWidth  / szPx.width;
    const double scaleY = (double)kGuestHeight / szPx.height;

    const uint32_t x = (uint32_t)std::clamp(ptPx.x * scaleX,
                                             0.0, (double)(kGuestWidth  - 1));
    const uint32_t y = (uint32_t)std::clamp((szPx.height - ptPx.y) * scaleY,
                                             0.0, (double)(kGuestHeight - 1));

    const bool left  = ([NSEvent pressedMouseButtons] & (1 << 0)) != 0;
    const bool right = ([NSEvent pressedMouseButtons] & (1 << 1)) != 0;

    g_vm->sendPointerEvent(x, y, left, right);
}

- (void)mouseMoved:(NSEvent *)event     { [self _handlePointerEvent:event]; }
- (void)mouseDown:(NSEvent *)event      { [self _handlePointerEvent:event]; }
- (void)mouseUp:(NSEvent *)event        { [self _handlePointerEvent:event]; }
- (void)mouseDragged:(NSEvent *)event   { [self _handlePointerEvent:event]; }
- (void)rightMouseDown:(NSEvent *)e     { [self _handlePointerEvent:e]; }
- (void)rightMouseUp:(NSEvent *)e       { [self _handlePointerEvent:e]; }
- (void)rightMouseDragged:(NSEvent *)e  { [self _handlePointerEvent:e]; }

@end

// ── AppDelegate ────────────────────────────────────────────────────────────────

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property (nonatomic, copy)   NSString  *kernelPath;
@property (nonatomic, copy)   NSString  *diskPath;
@property (nonatomic, copy)   NSString  *initrdPath;
@property (nonatomic, strong) NSWindow  *window;
@property (nonatomic, strong) FrameView *frameView;
@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)__unused note {
    NSRect content = NSMakeRect(0, 0, kGuestWidth, kGuestHeight);

    self.window = [[NSWindow alloc]
        initWithContentRect:content
                  styleMask:  NSWindowStyleMaskTitled
                            | NSWindowStyleMaskClosable
                            | NSWindowStyleMaskMiniaturizable
                            | NSWindowStyleMaskResizable
                  backing:NSBackingStoreBuffered
                    defer:NO];
    self.window.title    = @"sample-vm-display";
    self.window.delegate = self;

    self.frameView = [[FrameView alloc] initWithFrame:content];
    self.window.contentView = self.frameView;
    [self.window makeFirstResponder:self.frameView];

    [self.window center];
    [self.window makeKeyAndOrderFront:nil];

    dispatch_async(
        dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0),
        ^{ [self runVM]; });
}

- (void)runVM {
    virtualization::LinuxBootLoader boot{self.kernelPath.UTF8String};
    if (self.initrdPath)
        boot.initrdURL = self.initrdPath.UTF8String;

    boot.commandLine =
        "earlycon=pl011,0x9000000 console=ttyAMA0 console=hvc0 "
        "root=/dev/vda rw panic=-1 loglevel=8 "
        "video=1280x800 drm.fbdev_emulation=1";

    virtualization::DiskImageStorageDeviceAttachment disk{
        self.diskPath.UTF8String, /*readOnly=*/false};
    virtualization::VirtioBlockDeviceConfiguration block_dev{disk};

    virtualization::NATNetworkDeviceAttachment nat{};
    nat.addPortForward({/*hostPort=*/2222, /*guestPort=*/22});
    virtualization::VirtioNetworkDeviceConfiguration net_dev{nat};

    virtualization::FileHandleSerialPortAttachment console_attach{
        virtualization::FileHandle::standardInput(),
        virtualization::FileHandle::standardOutput()};
    virtualization::VirtioConsoleDeviceSerialPortConfiguration console_dev{
        console_attach};

    virtualization::VirtioInputDeviceConfiguration kbd_cfg;
    kbd_cfg.type = virtualization::InputDeviceType::Keyboard;

    virtualization::VirtioInputDeviceConfiguration ptr_cfg;
    ptr_cfg.type   = virtualization::InputDeviceType::Tablet;
    ptr_cfg.width  = kGuestWidth;
    ptr_cfg.height = kGuestHeight;

    __weak FrameView     *weakView   = self.frameView;
    std::atomic<uint64_t> frameCount {0};

    virtualization::VirtioGPUDeviceConfiguration gpu_cfg{};
    gpu_cfg.renderer = virtualization::GPURenderer::Framebuffer2D;
    gpu_cfg.width    = kGuestWidth;
    gpu_cfg.height   = kGuestHeight;

    gpu_cfg.onFrameBufferUpdate =
        [weakView, &frameCount]
        (const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t stride)
    {
        const uint64_t n     = ++frameCount;
        const size_t   bytes = static_cast<size_t>(stride) * height;
        auto buf = std::make_shared<std::vector<uint8_t>>(pixels, pixels + bytes);

        dispatch_async(dispatch_get_main_queue(), ^{
            FrameView *view = weakView;
            if (!view) return;
            [view updateWithPixels:buf->data()
                             width:width
                            height:height
                            stride:stride];
            if (n == 1)
                fprintf(stderr, "[gpu] first frame — pipeline is live (%ux%u)\n",
                        width, height);
        });
    };

    virtualization::VirtualMachineConfiguration config;
    config.bootLoader = &boot;
    config.cpuCount   = 2;
    config.memorySize = 2 * virtualization::GiB;

    config.storageDevices.push_back(&block_dev);
    config.networkDevices.push_back(&net_dev);
    config.serialPorts.push_back(&console_dev);
    config.graphicsDevices.push_back(&gpu_cfg);
    config.inputDevices.push_back(&kbd_cfg);
    config.inputDevices.push_back(&ptr_cfg);

    try {
        config.validate();
    } catch (const std::invalid_argument& e) {
        fprintf(stderr, "Invalid configuration: %s\n", e.what());
        dispatch_async(dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
        return;
    }

    virtualization::VirtualMachine vm{config};
    g_vm = &vm;

    vm.start([](std::error_code ec) {
        if (ec)
            fprintf(stderr, "\n[vmm] VM stopped with error: %s\n",
                    ec.message().c_str());
        else
            fprintf(stderr, "\n[vmm] VM halted normally.\n");
    });

    fprintf(stderr, "[vmm] VM running — close the window to stop.\n");
    fprintf(stderr, "[vmm] SSH available at localhost:2222\n");

    vm.waitUntilStopped();
    g_vm = nullptr;

    fprintf(stderr, "[vmm] Total frames rendered: %llu\n",
            static_cast<unsigned long long>(frameCount.load()));

    dispatch_async(dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
}

- (void)windowWillClose:(NSNotification *)__unused note {
    if (g_vm) g_vm->stop();
}

- (void)windowDidBecomeKey:(NSNotification *)__unused note {
    [self.window makeFirstResponder:self.frameView];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)__unused app {
    return YES;
}

@end

// ── main ───────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    @autoreleasepool {
        if (argc < 3) {
            fprintf(stderr,
                    "Usage: %s <kernel-Image> <disk.img> [initrd]\n", argv[0]);
            return EXIT_FAILURE;
        }

        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        AppDelegate *delegate = [AppDelegate new];
        delegate.kernelPath   = @(argv[1]);
        delegate.diskPath     = @(argv[2]);
        delegate.initrdPath   = (argc >= 4) ? @(argv[3]) : nil;

        app.delegate = delegate;
        [app run];
    }
    return EXIT_SUCCESS;
}