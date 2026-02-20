#ifndef PUBDATATYPE_H
#define PUBDATATYPE_H

#include <cstddef>

struct FrameDesc
{
    int    index       = -1;       // 缓冲区索引，用于映射到 MPP 输入缓冲槽位
    int    fd          = -1;       // V4L2 导出的 dma-buf fd
    void*  base        = nullptr;  // mmap 后的基地址
    size_t Length      = 0;        // 缓冲区容量（字节）
    size_t payloadSize = 0;        // 当前帧有效载荷大小（字节）
};

#endif  // PUBDATATYPE_H
