# Public RAW dependencies

`libraw` adds the `dng-sdk` feature to the upstream vcpkg port. `shadow-dng-sdk` pins Adobe's
DNG 1.7.1 source archive and links the public `libjxl` dependency. SDK source and compiled
payloads stay in vcpkg's external download/build/install directories.

`shadow_dng_host.h` keeps Adobe class layout and build flags inside the dependency. The image
kernel owns a host per LibRaw session and destroys LibRaw before the host. SDK admission is
restricted to the JPEG XL codec; ordinary DNG, NEF and proprietary-provider routes retain their
existing development policy. JPEG XL uses its declared Stage 2/3 opcodes to restore camera-linear
pixels before white balance and development. Host ownership does not load a private camera provider.

The overlay configuration is registered in the root `vcpkg-configuration.json`. A classic-mode
development install must also pass `--overlay-ports=/absolute/path/to/shadow/cmake/vcpkg-ports`
and select `libraw[dng-sdk]`. Use task-private external install and build directories.

This product includes DNG technology under license by Adobe.
