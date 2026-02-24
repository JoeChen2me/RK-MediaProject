#ifndef MPP_COMMON_UTILS_H
#define MPP_COMMON_UTILS_H

#include <cstddef>
#include <cstdint>

namespace mpp_common
{
// MPP 编解码共享的 dma_heap 路径。
inline constexpr const char* kSystemDmaHeapPath = "/dev/dma_heap/system";

inline uint32_t Align16(uint32_t value) { return (value + 15u) & ~15u; }

// 从帧尾部回扫 EOI(FFD9)，裁掉 V4L2 可能附带的对齐填充。
inline size_t FindJpegEffectiveSize(const uint8_t* data, size_t len)
{
    if (!data || len < 4)
    {
        return 0;
    }
    for (size_t i = len; i >= 2; --i)
    {
        if (data[i - 2] == 0xFF && data[i - 1] == 0xD9)
        {
            return i;
        }
    }
    return 0;
}
}  // namespace mpp_common

#endif  // MPP_COMMON_UTILS_H
