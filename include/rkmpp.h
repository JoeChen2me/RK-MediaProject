#ifndef RKMPP_H
#define RKMPP_H

#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_mpi_cmd.h>

#include "pubDataType.h"  //导入公共数据类型头文件
#include <cstddef>
#include <iostream>
#include <map>
#include <vector>
namespace
{
constexpr size_t kMaxBufferCount   = 15U;  // MPP 输出缓冲区的数量
constexpr size_t kMaxImportBuffers = 15U;  // MPP 输入缓冲区的最大导入数量

};  // namespace

class MppInstance
{
   public:
    MppInstance();
    ~MppInstance();

    int MppInit();
    int MppAllocBuffer(const FrameDesc* frame_desc_array, size_t buffer_count);
    int MppConfigWidthHeight(uint32_t width, uint32_t height);
    int MppDecode(const FrameDesc* frame_desc);

    MppBuffer MppBuffers[kMaxImportBuffers] = {nullptr};  // 存储导入的输入缓冲句柄

   private:
    int  AllocDmaBufFD(MppOutputFD& output, size_t size);  // 分配并映射一个 dma-buf
    int  CommitExternalOutputBuffers(size_t size);         // 按指定大小提交外部输出缓冲到 group
    void ReleaseExternalOutputBuffers();                   // 释放外部输出缓冲 fd

    MppCtx         mpp_ctx                          = nullptr;  // 复用的 MPP 解码上下文
    MppApi*        mpp_api                          = nullptr;  // 复用的 MPP API 入口
    MppBufferGroup group                            = nullptr;  // task 模式下用于申请输出帧缓存
    uint32_t       ImgWidth                         = 0;        // 输入图像宽
    uint32_t       ImgHeight                        = 0;        // 输入图像高
    uint32_t       H_Stride                         = 0;        // 行对齐
    uint32_t       V_Stride                         = 0;        // 列对齐
    size_t         OutSize                          = 0;        // 单帧输出缓冲大小
    MppOutputFD    MppOutputFDList[kMaxBufferCount] = {};  // 存储输出缓冲的详细信息,包含完整信息
    std::map<int, size_t> OutBufFD2Index_Map;  // 记录输出 dma-buf fd 到缓冲索引的映射关系
};

#endif  // RKMPP_H
