


virtualization-framework/
│
├── include/
│   └── virtualization/
│       ├── virtualization.hpp
│       ├── virtio/
│       │   ├── block.hpp
│       │   ├── console.hpp
│       │   ├── input.hpp
│       │   └── net.hpp
│       ├── backend/
│       │   ├── hvf/
│       │   │   ├── vcpu.hpp
│       │   │   └── memory.hpp
│       │   ├── kvm/
│       │   │   ├── vcpu.hpp
│       │   │   └── memory.hpp
│       │   └── whp/
│       │       ├── vcpu.hpp
│       │       └── memory.hpp
│       ├── firmware/
│       │   ├── arm64/
│       │   │   ├── dtb.hpp
│       │   │   ├── gic.hpp
│       │   │   └── psci.hpp
│       │   └── x86_64/
│       │       ├── acpi.hpp
│       │       ├── apic.hpp
│       │       └── msr.hpp
│       ├── loader.hpp
│       └── logger.hpp
│
├── src/
│   ├── virtio/
│   │   ├── block.cpp
│   │   ├── input.cpp
│   │   ├── console.cpp
│   │   └── net.cpp
│   ├── netdev/
│   ├── core/
│   │   ├── vm.cpp
│   │   └── loader.cpp
│   ├── backend/
│   │   ├── hvf/
│   │   │   ├── vcpu.cpp
│   │   │   ├── memory.cpp
│   │   │   └── exit_handler.cpp
│   │   ├── kvm/
│   │   │   ├── vcpu.cpp
│   │   │   ├── memory.cpp
│   │   │   └── exit_handler.cpp
│   │   └── whp/
│   │       ├── vcpu.cpp
│   │       ├── memory.cpp
│   │       └── exit_handler.cpp
│   └── firmware/
│       ├── arm64/
│       │   ├── dtb.cpp
│       │   ├── gic.cpp
│       │   └── psci.cpp
│       └── x86_64/
│           ├── acpi.cpp
│           ├── apic.cpp
│           └── msr.cpp
│
└── CMakeLists.txt