# CUDA image execution

This owner supplies the Windows CUDA executor and resident warm-preview resources. CMake enables
it with `SHADOW_ENABLE_CUDA`; portable builds keep the CPU implementation available.

`generate_shaders.py` adapts the existing pixel-local shader interpreter and display functions to
CUDA syntax. It does not replace color algorithms: host lowering, resource layout, node order,
curve/LUT data and display semantics remain shared with the Metal implementation. `shader_types.cuh`
implements the small vector/address-space compatibility boundary. Generated headers stay in the
external build directory.

The immutable scene-linear source is uploaded once per warm session. A parameter change lowers
the current node plan and reruns its adjustment/display suffix without reopening the source or
repeating RAW development. Device intermediates stay resident. Packed RGB8 crosses to the host
at the portable Qt presentation/JPEG boundary; only settled analysis also reads linear floats.
Unsupported neighborhood, mask or geometry plans replay the complete stage from the immutable
CPU source. This is an explicit performance limitation, with identical Recipe meaning.

Each session owns bounded resources and observes cancellation before submission and after device
completion. The caller's revision gate remains responsible for publication. CUDA receipts have a
distinct backend identity; device names and diagnostic strings are never cache identities.
