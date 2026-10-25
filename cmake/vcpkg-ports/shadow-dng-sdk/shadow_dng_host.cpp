#include "shadow_dng_host.h"
#include "dng_host.h"

extern "C" void* shadow_dng_host_create() {
    return new dng_host;
}

extern "C" void shadow_dng_host_destroy(void* host) noexcept {
    delete static_cast<dng_host*>(host);
}
