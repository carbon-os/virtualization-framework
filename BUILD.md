# Building

## Requirements

- CMake 3.25+
- A C++20 compiler (Clang, GCC, or MSVC)
- Platform hypervisor support:
  - **macOS** — Hypervisor.framework (Apple Silicon or Intel, macOS 11+)
  - **Linux** — KVM (`/dev/kvm` must be accessible)
  - **Windows** — Windows Hypervisor Platform enabled in optional features

## Build

```sh
cmake -B build
cmake --build build
```


mkdir -p ~/.config && echo -e "[core]\nrequire-input=false" > ~/.config/weston.ini && weston --backend=drm-backend.so --use-pixman


The sample binary will be at `build/sample-vm`.

## Options

| Option                    | Default | Description                    |
|---------------------------|---------|--------------------------------|
| `VIRT_BUILD_SAMPLES`      | `ON`    | Build `sample-vm`              |
| `VIRT_BUILD_SHARED`       | `OFF`   | Build as a shared library      |
| `VIRT_ENABLE_NETDEV`      | `ON`    | Build network device backends  |
| `VIRT_WARNINGS_AS_ERRORS` | `OFF`   | Treat compiler warnings as errors |

## Running the sample

```sh
build/sample-vm <kernel-Image> <disk.img> [initrd]
```