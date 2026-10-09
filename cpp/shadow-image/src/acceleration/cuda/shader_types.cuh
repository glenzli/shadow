#pragma once
#include <cmath>
#include <cuda_runtime.h>

// Only the value/vector operations used by the shared pixel interpreter belong here.
namespace shadow_cuda_shader {
using uint = unsigned int;
using uchar = unsigned char;
using atomic_uint = uint;
constexpr int memory_order_relaxed = 0;
struct float2 {
    float x, y;
    float2() = default;
    __device__ explicit float2(float v) : x(v), y(v) {}
    __device__ float2(float a, float b) : x(a), y(b) {}
};
struct float2_reference {
    float &x, &y;
    __device__ operator float2() const {
        return float2(x, y);
    }
    __device__ void operator=(float2 v) {
        x = v.x;
        y = v.y;
    }
    __device__ void operator*=(float v) {
        x *= v;
        y *= v;
    }
};
struct uint3;
struct float3 {
    union {
        struct {
            float x, y, z;
        };
        struct {
            float r, g, b;
        };
    };
    float3() = default;
    __device__ explicit float3(uint3 v);
    __device__ explicit float3(float v) : x(v), y(v), z(v) {}
    __device__ float3(float a, float b_, float c) : x(a), y(b_), z(c) {}
    __device__ float2_reference yz() {
        return {y, z};
    }
    __device__ float2 yz() const {
        return float2(y, z);
    }
    __device__ float& operator[](uint i) {
        return (&x)[i];
    }
    __device__ float operator[](uint i) const {
        return (&x)[i];
    }
    __device__ void operator*=(float v) {
        x *= v;
        y *= v;
        z *= v;
    }
    __device__ void operator+=(float3 v) {
        x += v.x;
        y += v.y;
        z += v.z;
    }
};
struct alignas(16) float4 {
    union {
        struct {
            float x, y, z, w;
        };
        struct {
            float r, g, b, a;
        };
    };
    float4() = default;
    __device__ explicit float4(float v) : x(v), y(v), z(v), w(v) {}
    __device__ float4(float a_, float b_, float c, float d) : x(a_), y(b_), z(c), w(d) {}
    __device__ float4(float3 v, float d) : x(v.x), y(v.y), z(v.z), w(d) {}
    __device__ float2 xy() const {
        return float2(x, y);
    }
    __device__ float3 xyz() const {
        return float3(x, y, z);
    }
    __device__ float3 rgb() const {
        return xyz();
    }
    __device__ float3 yzw() const {
        return float3(y, z, w);
    }
    __device__ void operator+=(float4 v) {
        x += v.x;
        y += v.y;
        z += v.z;
        w += v.w;
    }
    __device__ float operator[](uint i) const {
        return (&x)[i];
    }
};
struct uint2 {
    uint x, y;
    __device__ uint2(uint a, uint b) : x(a), y(b) {}
    __device__ explicit uint2(float2 v) : x(uint(v.x)), y(uint(v.y)) {}
};
struct uint3 {
    uint x, y, z;
    __device__ explicit uint3(uint v) : x(v), y(v), z(v) {}
    __device__ uint3(uint a, uint b, uint c) : x(a), y(b), z(c) {}
    __device__ explicit uint3(float3 v) : x(uint(v.x)), y(uint(v.y)), z(uint(v.z)) {}
};
__device__ inline float3::float3(uint3 v) : x(float(v.x)), y(float(v.y)), z(float(v.z)) {}
struct uint4 {
    uint x, y, z, w;
    __device__ explicit uint4(float4 v) : x(uint(v.x)), y(uint(v.y)), z(uint(v.z)), w(uint(v.w)) {}
};
struct bool3 {
    bool x, y, z;
};
struct bool4 {
    bool x, y, z, w;
};
#define SHADOW_CUDA_VEC3_OP(op)                                                                    \
    __device__ inline float3 operator op(float3 a, float3 b) {                                     \
        return float3(a.x op b.x, a.y op b.y, a.z op b.z);                                         \
    }                                                                                              \
    __device__ inline float3 operator op(float3 a, float b) {                                      \
        return a op float3(b);                                                                     \
    }                                                                                              \
    __device__ inline float3 operator op(float a, float3 b) {                                      \
        return float3(a) op b;                                                                     \
    }
SHADOW_CUDA_VEC3_OP(+)
SHADOW_CUDA_VEC3_OP(-)
SHADOW_CUDA_VEC3_OP(*)
SHADOW_CUDA_VEC3_OP(/)
#undef SHADOW_CUDA_VEC3_OP
__device__ inline float2 operator/(float2 a, float b) {
    return float2(a.x / b, a.y / b);
}
__device__ inline float2 operator*(float2 a, float b) {
    return float2(a.x * b, a.y * b);
}
__device__ inline float4 operator*(float4 a, float b) {
    return float4(a.x * b, a.y * b, a.z * b, a.w * b);
}
__device__ inline float4 operator*(float a, float4 b) {
    return b * a;
}
__device__ inline float4 operator*(float4 a, float4 b) {
    return float4(a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w);
}
__device__ inline float4 operator+(float4 a, float4 b) {
    return float4(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w);
}
__device__ inline uint3 operator+(uint3 a, uint3 b) {
    return uint3(a.x + b.x, a.y + b.y, a.z + b.z);
}
__device__ inline uint3 min(uint3 a, uint3 b) {
    return uint3(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z);
}
__device__ inline bool3 operator<(float3 a, float3 b) {
    return {a.x < b.x, a.y < b.y, a.z < b.z};
}
__device__ inline bool3 operator<=(float3 a, float b) {
    return {a.x <= b, a.y <= b, a.z <= b};
}
__device__ inline bool all(bool3 v) {
    return v.x && v.y && v.z;
}
__device__ inline bool all(bool4 v) {
    return v.x && v.y && v.z && v.w;
}
__device__ inline bool4 operator==(float4 a, float4 b) {
    return {a.x == b.x, a.y == b.y, a.z == b.z, a.w == b.w};
}
__device__ inline bool3 isfinite(float3 v) {
    return {::isfinite(v.x), ::isfinite(v.y), ::isfinite(v.z)};
}
__device__ inline bool isfinite(float v) {
    return ::isfinite(v);
}
__device__ inline float min(float a, float b) {
    return fminf(a, b);
}
__device__ inline float max(float a, float b) {
    return fmaxf(a, b);
}
__device__ inline uint min(uint a, uint b) {
    return a < b ? a : b;
}
__device__ inline float abs(float v) {
    return fabsf(v);
}
__device__ inline float clamp(float v, float a, float b) {
    return min(max(v, a), b);
}
__device__ inline float3 clamp(float3 v, float a, float b) {
    return float3(clamp(v.x, a, b), clamp(v.y, a, b), clamp(v.z, a, b));
}
__device__ inline float3 clamp(float3 v, float3 a, float3 b) {
    return float3(clamp(v.x, a.x, b.x), clamp(v.y, a.y, b.y), clamp(v.z, a.z, b.z));
}
__device__ inline float4 clamp(float4 v, float a, float b) {
    return float4(clamp(v.x, a, b), clamp(v.y, a, b), clamp(v.z, a, b), clamp(v.w, a, b));
}
__device__ inline float dot(float3 a, float3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
__device__ inline float length(float2 a) {
    return sqrtf(a.x * a.x + a.y * a.y);
}
__device__ inline float3 floor(float3 v) {
    return float3(floorf(v.x), floorf(v.y), floorf(v.z));
}
__device__ inline float floor(float v) {
    return floorf(v);
}
__device__ inline float pow(float v, float p) {
    return powf(v, p);
}
__device__ inline float3 pow(float3 v, float3 p) {
    return float3(powf(v.x, p.x), powf(v.y, p.y), powf(v.z, p.z));
}
__device__ inline float3 select(float3 a, float3 b, bool3 p) {
    return float3(p.x ? b.x : a.x, p.y ? b.y : a.y, p.z ? b.z : a.z);
}
__device__ inline float mix(float a, float b, float t) {
    return a * (1.0f - t) + b * t;
}
__device__ inline float3 mix(float3 a, float3 b, float t) {
    return a * (1.0f - t) + b * t;
}
__device__ inline float2 mix(float2 a, float2 b, float t) {
    return float2(mix(a.x, b.x, t), mix(a.y, b.y, t));
}
__device__ inline void atomic_fetch_or_explicit(uint* p, uint v, int) {
    atomicOr(p, v);
}
__device__ inline void atomic_fetch_min_explicit(uint* p, uint v, int) {
    atomicMin(p, v);
}
template <typename T> __device__ T as_type(uint v);
template <> __device__ inline float as_type<float>(uint v) {
    return __uint_as_float(v);
}
} // namespace shadow_cuda_shader
