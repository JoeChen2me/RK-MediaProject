#ifndef RKMPP_H
#define RKMPP_H

#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_mpi_cmd.h>

class MppInstance
{
   public:
    MppInstance();
    ~MppInstance();

    int MppInit();
    int MppAllocBuffer();
    int MppConfigWidthHeight(uint32_t width, uint32_t height);
    int MppDecode(const uint8_t* data, size_t len);

   private:
    MppCtx         mpp_ctx   = nullptr;  // 复用的 MPP 解码上下文
    MppApi*        mpp_api   = nullptr;  // 复用的 MPP API 入口
    MppBufferGroup group     = nullptr;  // 模式1内部缓冲组（复用）
    uint32_t       ImgWidth  = 0;        // 输入图像宽
    uint32_t       ImgHeight = 0;        // 输入图像高
    uint32_t       H_Stride  = 0;        // 行对齐
    uint32_t       V_Stride  = 0;        // 列对齐
    size_t         OutSize   = 0;        // 单帧输出缓冲大小
};

#endif  // RKMPP_H
