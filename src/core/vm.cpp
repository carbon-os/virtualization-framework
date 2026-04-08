#include "virtualization/virtualization.hpp"
#include "virtualization/vm_context.hpp"
#include "virtualization/backend/hvf/memory.hpp"
#include "virtualization/backend/hvf/vcpu.hpp"
#include "virtualization/firmware/arm64/dtb.hpp"
#include "virtualization/loader.hpp"
#include "virtualization/logger.hpp"
#include "virtualization/virtio/block.hpp"
#include "virtualization/virtio/console.hpp"
#include "virtualization/virtio/gpu.hpp"
#include "virtualization/virtio/net.hpp"
#include "virtualization/virtio/input.hpp"
#include <Hypervisor/Hypervisor.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/shm.h>
#include <thread>
#include <unistd.h>
#include <cstdio>

namespace virtualization {

FileHandle FileHandle::standardInput()  noexcept { return FileHandle{STDIN_FILENO};  }
FileHandle FileHandle::standardOutput() noexcept { return FileHandle{STDOUT_FILENO}; }

MACAddress::MACAddress(std::string_view str) {
    unsigned int b[6];
    if (std::sscanf(str.data(), "%x:%x:%x:%x:%x:%x",
                    &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        throw std::invalid_argument("MACAddress: expected format \"xx:xx:xx:xx:xx:xx\"");
    }
    for (int i = 0; i < 6; ++i) bytes_[i] = static_cast<uint8_t>(b[i]);
}

MACAddress::MACAddress(uint8_t b0, uint8_t b1, uint8_t b2,
                       uint8_t b3, uint8_t b4, uint8_t b5) noexcept {
    bytes_[0]=b0; bytes_[1]=b1; bytes_[2]=b2;
    bytes_[3]=b3; bytes_[4]=b4; bytes_[5]=b5;
}

void VirtualMachineConfiguration::validate() const {
    if (!bootLoader) throw std::invalid_argument("bootLoader must not be null");
    if (cpuCount == 0 || cpuCount > kVcpuMax) throw std::invalid_argument("cpuCount must be in [1, kVcpuMax]");
    if (memorySize < 128 * MiB) throw std::invalid_argument("memorySize must be at least 128 MiB");
    if (memorySize & (MiB - 1)) throw std::invalid_argument("memorySize must be a multiple of 1 MiB");
}

struct VirtualMachine::Impl {
    std::unique_ptr<VMContext> ctx;
    std::function<void(std::error_code)> completion;
    std::thread run_thread;

    explicit Impl(const VirtualMachineConfiguration& config) {
        auto* lb = dynamic_cast<LinuxBootLoader*>(config.bootLoader);
        if (!lb) throw std::runtime_error("Only LinuxBootLoader is supported");

        ctx = std::make_unique<VMContext>(config.cpuCount, config.memorySize);
        if (!lb->commandLine.empty()) ctx->set_cmdline(lb->commandLine);

        if (hv_vm_create(nullptr) != HV_SUCCESS) throw std::runtime_error("hv_vm_create failed");

        if (!Memory::init(*ctx)) throw std::runtime_error("Memory::init failed");

        if (!ctx->create_console()) throw std::runtime_error("create_console failed");

        for (auto* blk : config.storageDevices) {
            if (!blk || !blk->attachment) continue;
            auto* di = dynamic_cast<DiskImageStorageDeviceAttachment*>(blk->attachment);
            if (!di) continue;
            if (!ctx->create_block(di->imageURL)) throw std::runtime_error("create_block failed");
            break;
        }

        for (auto* net : config.networkDevices) {
            if (!net || !net->attachment) continue;
            auto* nat = dynamic_cast<NATNetworkDeviceAttachment*>(net->attachment);
            if (!nat) continue;
            NetDeviceConfig nc;
            nc.enabled = true;
            const uint8_t* m = net->macAddress.bytes();
            for (int i = 0; i < 6; ++i) nc.mac[i] = m[i];
            for (const auto& pf : nat->portForwards)
                nc.port_forwards.push_back({pf.hostPort, pf.guestPort});
            if (!ctx->create_net(nc)) throw std::runtime_error("create_net failed");
            break;
        }

        // --- Input devices ---
        for (auto* in : config.inputDevices) {
            if (!in) continue;
            if (in->type == InputDeviceType::Keyboard) {
                if (!ctx->create_keyboard()) throw std::runtime_error("create_keyboard failed");
            } else if (in->type == InputDeviceType::Tablet) {
                if (!ctx->create_tablet(in->width, in->height)) throw std::runtime_error("create_tablet failed");
            }
        }

        if (!config.graphicsDevices.empty() && config.graphicsDevices[0]) {
            const auto* gcfg = config.graphicsDevices[0];
            if (!ctx->create_gpu(gcfg->width, gcfg->height, gcfg->onFrameBufferUpdate))
                throw std::runtime_error("create_gpu failed");
        }

        if (!ctx->init_vcpus()) throw std::runtime_error("init_vcpus failed");

        if (!Loader::load_kernel(*ctx, lb->kernelURL)) throw std::runtime_error("load_kernel failed");

        if (!lb->initrdURL.empty()) {
            if (!Loader::load_initrd(*ctx, lb->initrdURL)) throw std::runtime_error("load_initrd failed");
        }

        if (!DTB::generate(*ctx)) throw std::runtime_error("DTB::generate failed");
    }

    ~Impl() {
        if (run_thread.joinable()) {
            ctx->stop();
            run_thread.join();
        }
        hv_vm_destroy();
        logger::shutdown();
    }
};

VirtualMachine::VirtualMachine(const VirtualMachineConfiguration& config)
    : impl_(std::make_unique<Impl>(config)) {}

VirtualMachine::~VirtualMachine() = default;

void VirtualMachine::start(std::function<void(std::error_code)> completionHandler) {
    impl_->completion = std::move(completionHandler);
    impl_->run_thread = std::thread([this] {
        impl_->ctx->run_all();
        if (impl_->completion) impl_->completion(std::error_code{});
    });
}

void VirtualMachine::stop() { if (impl_) impl_->ctx->stop(); }

void VirtualMachine::waitUntilStopped() {
    if (impl_ && impl_->run_thread.joinable()) impl_->run_thread.join();
}

// ── VM Public Event APIs ──────────────────────────────────────────────────────

void VirtualMachine::sendKeyboardEvent(uint16_t linux_keycode, bool pressed) {
    if (impl_ && impl_->ctx && impl_->ctx->keyboard()) {
        impl_->ctx->keyboard()->send_key(linux_keycode, pressed);
    }
}

void VirtualMachine::sendPointerEvent(uint32_t x, uint32_t y, bool btn_left, bool btn_right) {
    if (impl_ && impl_->ctx && impl_->ctx->tablet()) {
        impl_->ctx->tablet()->send_pointer(x, y, btn_left, btn_right);
    }
}

// ── VMContext supplementary definitions ───────────────────────────────────────

VMContext::VMContext(uint32_t vcpu_count, uint64_t ram_size) noexcept
    : ram_size_(ram_size), vcpu_count_(vcpu_count) {}

VMContext::~VMContext() {
    vcpus_.clear();
    tablet_.reset();
    keyboard_.reset();
    gpu_.reset();
    net_.reset();
    block_.reset();
    console_.reset();

    if (ram_hva_) {
        hv_vm_unmap(ram_gpa_, ram_size_);
        ::munmap(ram_hva_, ram_size_);
        ram_hva_ = nullptr;
    }
    if (!ram_shm_path_.empty()) ::shm_unlink(ram_shm_path_.c_str());
}

void* VMContext::gpa_to_hva(uint64_t gpa) const noexcept {
    if (ram_hva_ && gpa >= ram_gpa_ && gpa < ram_gpa_ + ram_size_)
        return static_cast<uint8_t*>(ram_hva_) + (gpa - ram_gpa_);
    return nullptr;
}

void VMContext::set_ram(void* hva, uint64_t gpa, uint64_t size, std::string shm_path) noexcept {
    ram_hva_ = hva; ram_gpa_ = gpa; ram_size_ = size; ram_shm_path_ = std::move(shm_path);
}

bool VMContext::init_vcpus() {
    vcpus_.reserve(vcpu_count_);
    for (uint32_t i = 0; i < vcpu_count_; ++i)
        vcpus_.push_back(std::make_unique<VCpu>(*this, i));
    return true;
}

bool VMContext::create_console() {
    console_ = std::make_unique<virtio::Console>(*this, kGpaVirtioConsole);
    return console_ != nullptr;
}

bool VMContext::create_block(std::string_view image_path) {
    std::string p{image_path};
    block_ = std::make_unique<virtio::Block>(*this, kGpaVirtioBlock, p.c_str());
    if (!block_->valid()) logger::log("[blk] no disk image — /dev/vda unavailable\n");
    return true;
}

bool VMContext::create_net(const NetDeviceConfig& cfg) {
    if (!cfg.enabled) return true;
    net_ = std::make_unique<virtio::Net>(*this, kGpaVirtioNet, cfg);
    return true;
}

bool VMContext::create_gpu(uint32_t width, uint32_t height, GpuFrameCallback cb) {
    gpu_ = std::make_unique<virtio::Gpu>(*this, kGpaVirtioGpu, width, height, std::move(cb));
    return true;
}

bool VMContext::create_keyboard() {
    keyboard_ = std::make_unique<virtio::Input>(
        *this, kGpaVirtioKeyboard, kSpiKeyboard,
        virtio::Input::Type::Keyboard);
    return true;
}

bool VMContext::create_tablet(uint32_t width, uint32_t height) {
    tablet_ = std::make_unique<virtio::Input>(
        *this, kGpaVirtioTablet, kSpiTablet,
        virtio::Input::Type::Tablet, width, height);
    return true;
}

void VMContext::run_all() {
    running_.store(true, std::memory_order_relaxed);
    for (auto& v : vcpus_) v->spawn();

    while (running_.load(std::memory_order_relaxed)) {
        ::usleep(1000);
    }

    for (auto& v : vcpus_) v->join();
    running_.store(false, std::memory_order_relaxed);
}

} // namespace virtualization