#ifndef PUBDATATYPE_H
#define PUBDATATYPE_H

#include <cstddef>
#include <cstdint>

namespace resource_limits
{
// 全局资源池上限，按未来 H.264/H.265 参考帧场景预留。
inline constexpr size_t kMaxAllocaSize = 35U;

// 相机、MPP、RGA 的资源规模统一在此集中配置。
inline constexpr size_t kCameraRequestBufferCount = 4U;
inline constexpr size_t kCameraMappedBufferCount  = kMaxAllocaSize;
inline constexpr size_t kMppImportBufferCount     = kMaxAllocaSize;
inline constexpr size_t kMppOutputBufferCount     = kMaxAllocaSize;
inline constexpr size_t kRgaOutputBufferCount     = kMaxAllocaSize;
}  // namespace resource_limits

// 用于在相机和 MPP 之间传递帧缓冲描述信息，包含必要的 fd、尺寸和有效载荷大小等字段。
struct FrameDesc
{
    int    index       = -1;       // 缓冲区索引，用于映射到 MPP 输入缓冲槽位
    int    fd          = -1;       // V4L2 导出的 dma-buf fd
    void*  base        = nullptr;  // mmap 后的基地址
    size_t Length      = 0;        // 缓冲区容量（字节）
    size_t payloadSize = 0;        // 当前帧有效载荷大小（字节）
};
// 统一的可用于输入/输出的 DMA-BUF 描述结构，简化接口定义和使用。
struct IO_FD_t
{
    int      fd         = -1;       // dma-buf fd
    void*    base       = nullptr;  // 可选的 mmap 基地址，便于调试或特殊处理
    size_t   size       = 0;        // 缓冲区大小（字节）
    uint32_t width      = 0;        // 图像宽度（像素）
    uint32_t height     = 0;        // 图像高度（像素）
    uint32_t hor_stride = 0;        // 图像水平 stride（像素）
    uint32_t ver_stride = 0;        // 图像垂直 stride（像素）
};

struct VideoCaptureConfig
{
    uint32_t width        = 0;  // 实际生效宽度（由驱动返回）
    uint32_t height       = 0;  // 实际生效高度（由驱动返回）
    uint32_t pixelformat  = 0;  // V4L2 四字符编码（如 V4L2_PIX_FMT_MJPEG）
    uint32_t bytesperline = 0;  // 生效行跨度
    uint32_t sizeimage    = 0;  // 单帧缓冲推荐大小
    uint32_t fps_num      = 0;  // timeperframe 分子
    uint32_t fps_den      = 0;  // timeperframe 分母
    bool     valid        = false;
};

#endif  // PUBDATATYPE_H
