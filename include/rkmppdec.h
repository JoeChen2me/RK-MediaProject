#ifndef RKMPPDEC_H
#define RKMPPDEC_H

#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_mpi_cmd.h>

#include "pubDataType.h"  //导入公共数据类型头文件
#include <array>
#include <cstddef>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <vector>

class MppDecInstance
{
   public:
    MppDecInstance();
    ~MppDecInstance();

    int DecInit();
    int DecAllocBuffer(const FrameDesc* frame_desc_array, size_t buffer_count);
    int DecConfigWidthHeight(uint32_t width, uint32_t height);
    int MppDecode(const FrameDesc* frame_desc);
    int DecQueueOutputForRecycle(const IO_FD_t* output_desc);

    MppBuffer MppBuffers[resource_limits::kMppImportBufferCount] = {
        nullptr};                                // 存储导入的输入缓冲句柄
    const IO_FD_t* CurrentOutputDesc = nullptr;  // 当前解码完成并可供后续消费的输出描述

   private:
    struct DecodedTaskHolder  // 封装 DMABUF 与解码输出资源的结构体
    {
        IO_FD_t* output_desc = nullptr;  // 输出缓冲的描述信息，包含 fd、尺寸、格式等
        MppFrame output_frame = nullptr;  // 解码输出的 MppFrame 资源，关联了输出缓冲
        MppBuffer output_buf = nullptr;  // 解码输出的 MppBuffer 资源，关联了输出缓冲
    };

    int AllocDmaBufFD(IO_FD_t& output, size_t size);  // 分配并映射一个 dma-buf
    int CommitExternalOutputBuffers(size_t size);  // 按指定大小提交外部输出缓冲到 group
    void               ReleaseExternalOutputBuffers();  // 释放外部输出缓冲 fd
    DecodedTaskHolder* AcquireDecodedTaskHolder();
    void               ReturnDecodedTaskHolder(DecodedTaskHolder* holder);
    void               ReleaseTaskPacket(MppTask task);
    void               RecycleDecodedTaskHolder(DecodedTaskHolder* holder);
    void               DrainPendingRecycleQueue();
    void               ForceRecycleAllHolders();

    MppCtx         mpp_ctx   = nullptr;  // 复用的 MPP 解码上下文
    MppApi*        mpp_api   = nullptr;  // 复用的 MPP API 入口
    MppBufferGroup group     = nullptr;  // task 模式下用于申请输出帧缓存
    uint32_t       ImgWidth  = 0;        // 输入图像宽
    uint32_t       ImgHeight = 0;        // 输入图像高
    uint32_t       H_Stride  = 0;        // 行对齐
    uint32_t       V_Stride  = 0;        // 列对齐
    size_t         OutSize   = 0;        // 单帧输出缓冲大小
    IO_FD_t        MppOutputFDList[resource_limits::kMppOutputBufferCount] =
        {};  // 存储输出缓冲的详细信息,包含完整信息
    std::map<int, size_t> OutBufFD2Index_Map;  // 记录输出 dma-buf fd 到缓冲索引的映射关系
    std::array<DecodedTaskHolder, resource_limits::kMppOutputBufferCount> HolderPool =
        {};  // 固定资源池，避免频繁 new/delete
    std::deque<DecodedTaskHolder*>               FreeHolderQueue;
    std::deque<DecodedTaskHolder*>               PendingRecycleQueue;
    std::map<const IO_FD_t*, DecodedTaskHolder*> OutDesc2HolderMap;  // 按 IO_FD_t* 回收资源
    std::mutex                                   HolderMutex;
};

#endif  // RKMPPDEC_H
