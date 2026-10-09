"""Generate CUDA syntax from Shadow's authoritative shared GPU pixel math."""
import argparse
import re
from pathlib import Path


def extract(path: Path) -> str:
    text = path.read_text(encoding="utf-8")
    return text.split('R"METAL(', 1)[1].split(')METAL"', 1)[0]


def adapt(source: str) -> str:
    source = source.replace("#include <metal_stdlib>", "")
    source = source.replace("using namespace metal;", "")
    source = re.sub(r"^constant (.+)$", r"__device__ constexpr \1", source, flags=re.M)
    source = re.sub(r"\bconstant\b", "const", source)
    source = re.sub(r"\b(device|thread)\s+", "", source)
    source = re.sub(r"\binline\b", "__device__ inline", source)
    source = re.sub(r"\.(xy|xyz|yz|yzw|rgb)\b", r".\1()", source)
    return source


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    common = extract(args.source / "edit/metal_adjustment_msl.hpp")
    warm = extract(args.source / "proxy/warm_edit_gpu_msl.hpp")
    display = warm.split("inline float3 linear_srgb_to_oklab", 1)[1]
    display = "inline float3 linear_srgb_to_oklab" + display.split("kernel void render_warm_preview_v1", 1)[0]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        "// Generated from shared GPU math; do not edit.\n#pragma once\n"
        '#include "shader_types.cuh"\nnamespace shadow_cuda_shader {\n'
        + adapt(common + display) + "\n}\n", encoding="utf-8"
    )


if __name__ == "__main__":
    main()
