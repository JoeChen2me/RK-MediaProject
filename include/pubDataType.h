#ifndef PUBDATATYPE_H
#define PUBDATATYPE_H

#include <cstddef>
#include <cstdint>

struct FrameDesc
{
    int    index       = -1;       // 缓冲区索引，用于映射到 MPP 输入缓冲槽位
    int    fd          = -1;       // V4L2 导出的 dma-buf fd
    void*  base        = nullptr;  // mmap 后的基地址
    size_t Length      = 0;        // 缓冲区容量（字节）
    size_t payloadSize = 0;        // 当前帧有效载荷大小（字节）
};

struct MppOutputFD
{
    int    fd   = -1;       // MPP 输出缓冲的 dma-buf fd
    void*  base = nullptr;  // 可选的 mmap 基地址，便于调试或特殊处理
    size_t size = 0;        // 缓冲区大小（字节）
};

struct VideoCaptureConfig
{
    uint32_t width       = 0;  // 实际生效宽度（由驱动返回）
    uint32_t height      = 0;  // 实际生效高度（由驱动返回）
    uint32_t pixelformat = 0;  // V4L2 四字符编码（如 V4L2_PIX_FMT_MJPEG）
    uint32_t bytesperline = 0; // 生效行跨度
    uint32_t sizeimage    = 0; // 单帧缓冲推荐大小
    uint32_t fps_num      = 0; // timeperframe 分子
    uint32_t fps_den      = 0; // timeperframe 分母
    bool     valid        = false;
};

#endif  // PUBDATATYPE_H
