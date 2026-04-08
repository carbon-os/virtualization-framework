#include "virtualization/firmware/arm64/gic.hpp"
#include "virtualization/logger.hpp"
#include <Hypervisor/Hypervisor.h>

namespace virtualization {

// Pulse SPI spi_number on the GICv3 emulated by Hypervisor.framework.
// The assert + deassert pattern is equivalent to an edge-triggered interrupt.
void raise_spi(uint32_t spi_number) noexcept {
    const uint32_t intid = kGicSpiBase + spi_number;
    const hv_return_t ra = hv_gic_set_spi(intid, true);
    const hv_return_t rd = hv_gic_set_spi(intid, false);
    if (ra != HV_SUCCESS || rd != HV_SUCCESS) {
        logger::log("[gic] raise_spi(%u) failed: assert=0x%x deassert=0x%x\n",
                    spi_number, ra, rd);
    }
}

} // namespace virtualization