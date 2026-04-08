// samples/main.cpp
//
// Minimal example: boot a Linux arm64 guest with a disk image,
// NAT networking (SSH forwarded to host port 2222), and a virtio
// console wired to stdin/stdout.

#include <virtualization/virtualization.hpp>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <system_error>

// ── Globals for signal handling ───────────────────────────────────

static virtualization::VirtualMachine* g_vm = nullptr;

static void on_signal(int) {
    if (g_vm) g_vm->stop();
}

// ─────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // ── Parse arguments ───────────────────────────────────────────
    // Usage: sample-vm <kernel> <disk.img> [initrd]
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <kernel-Image> <disk.img> [initrd]\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char* kernel_path = argv[1];
    const char* disk_path   = argv[2];
    const char* initrd_path = (argc >= 4) ? argv[3] : nullptr;

    // ── Boot loader ───────────────────────────────────────────────
    virtualization::LinuxBootLoader boot{kernel_path};
    if (initrd_path)
        boot.initrdURL = initrd_path;
    boot.commandLine =
        "earlycon=pl011,0x9000000 console=ttyAMA0 console=hvc0 root=/dev/vda rw "
        "panic=-1 loglevel=8";

    // ── Storage ───────────────────────────────────────────────────
    virtualization::DiskImageStorageDeviceAttachment disk{
        disk_path, /*readOnly=*/false};
    virtualization::VirtioBlockDeviceConfiguration block_dev{disk};

    // ── Network ───────────────────────────────────────────────────
    virtualization::NATNetworkDeviceAttachment nat{};
    nat.addPortForward({/*hostPort=*/2222, /*guestPort=*/22});

    virtualization::VirtioNetworkDeviceConfiguration net_dev{nat};

    // ── Console  (stdin → guest, guest → stdout) ──────────────────
    virtualization::FileHandleSerialPortAttachment console_attach{
        virtualization::FileHandle::standardInput(),
        virtualization::FileHandle::standardOutput()};
    virtualization::VirtioConsoleDeviceSerialPortConfiguration console_dev{
        console_attach};

    // ── Assemble configuration ────────────────────────────────────
    virtualization::VirtualMachineConfiguration config;
    config.bootLoader = &boot;
    config.cpuCount   = 2;
    config.memorySize = 2 * virtualization::GiB;

    config.storageDevices.push_back(&block_dev);
    config.networkDevices.push_back(&net_dev);
    config.serialPorts.push_back(&console_dev);

    try {
        config.validate();
    } catch (const std::invalid_argument& e) {
        fprintf(stderr, "Invalid configuration: %s\n", e.what());
        return EXIT_FAILURE;
    }

    // ── Create and start VM ───────────────────────────────────────
    virtualization::VirtualMachine vm{config};
    g_vm = &vm;

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    vm.start([](std::error_code ec) {
        if (ec)
            fprintf(stderr, "\n[vmm] VM stopped with error: %s\n", ec.message().c_str());
        else
            fprintf(stderr, "\n[vmm] VM halted normally.\n");
    });

    fprintf(stderr, "[vmm] VM running — press Ctrl-C to stop.\n");
    fprintf(stderr, "[vmm] SSH available at localhost:2222\n");

    vm.waitUntilStopped();

    g_vm = nullptr;
    return EXIT_SUCCESS;
}