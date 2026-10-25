#pragma once

// LibRaw accepts an opaque Adobe dng_host. Keep SDK compilation flags and class layout
// inside the dependency; each decoder owns one host until after LibRaw destruction.
extern "C" void* shadow_dng_host_create();
extern "C" void shadow_dng_host_destroy(void* host) noexcept;
