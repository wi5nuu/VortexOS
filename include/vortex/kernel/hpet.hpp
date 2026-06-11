#pragma once

#include "vortex/types.hpp"

namespace vortex::kernel::hpet {

void hpet_init(uintptr_t rsdp_addr);
uint64_t hpet_read_us();
void hpet_sleep_us(uint64_t us);

} // namespace vortex::kernel::hpet
