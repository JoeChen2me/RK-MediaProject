#include "rkmppdec.h"
#include "mpp_common_utils.h"

#include <fcntl.h>  // open, O_RDWR, O_CLOEXEC
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>  // close

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

namespace
{
constexpr MppPollType kPollTimeout500Ms   = static_cast<MppPollType>(500);
constexpr auto        kHolderWaitInterval = std::chrono::milliseconds(100);

bool dma_buf_sync_end_read(int fd)
{
    if (fd < 0)
    {
        return false;
    }
    dma_buf_sync sync{};
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    return ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) == 0;
}
}  // namespace

MppDecInstance::MppDecInstance()
{
    for (auto& fd_info : MppOutputFDList)
    {
        fd_info.fd         = -1;
        fd_info.base       = nullptr;
        fd_info.size       = 0;
        fd_info.format     = 0;
        fd_info.width      = 0;
        fd_info.height     = 0;
        fd_info.hor_stride = 0;
        fd_info.ver_stride = 0;
        fd_info.pts_us     = -1;
        fd_info.dts_us     = -1;
    }
    OutBufFD2Index_Map.clear();  // 初始化映射表
    CurrentOutputDesc = nullptr;
    for (auto& holder : HolderPool)
    {
        holder.output_desc  = nullptr;
        holder.output_frame = nullptr;
        holder.output_buf   = nullptr;
        FreeHolderQueue.push_back(&holder);
    }
    PendingRecycleQueue.clear();
    OutDesc2HolderMap.clear();
}

MppDecInstance::DecodedTaskHolder* MppDecInstance::AcquireDecodedTaskHolder()
{
    std::lock_guard<std::mutex> lock(HolderMutex);
    if (FreeHolderQueue.empty())
    {
        return nullptr;
    }
    DecodedTaskHolder* holder = FreeHolderQueue.front();
    FreeHolderQueue.pop_front();
    return holder;
}

void MppDecInstance::ReturnDecodedTaskHolder(DecodedTaskHolder* holder)
{
    if (!holder)
    {
        return;
    }
    holder->output_desc  = nullptr;
    holder->output_frame = nullptr;
    holder->output_buf   = nullptr;

    std::lock_guard<std::mutex> lock(HolderMutex);
    FreeHolderQueue.push_back(holder);
}

void MppDecInstance::ReleaseTaskPacket(MppTask task)
{
    if (!task)
    {
        return;
    }

    MppPacket packet_from_task = nullptr;
    if (mpp_task_meta_get_packet(task, KEY_INPUT_PACKET, &packet_from_task) == MPP_OK &&
        packet_from_task)
    {
        mpp_packet_deinit(&packet_from_task);
    }
}

void MppDecInstance::RecycleDecodedTaskHolder(DecodedTaskHolder* holder)
{
    if (!holder)
    {
        return;
    }

    if (holder->output_frame)
    {
        mpp_frame_deinit(&holder->output_frame);
        holder->output_frame = nullptr;
    }
    if (holder->output_buf)
    {
        mpp_buffer_put(holder->output_buf);
        holder->output_buf = nullptr;
    }
    ReturnDecodedTaskHolder(holder);
}

void MppDecInstance::DrainPendingRecycleQueue()
{
    std::deque<DecodedTaskHolder*> local_queue;
    {
        std::lock_guard<std::mutex> lock(HolderMutex);
        local_queue.swap(PendingRecycleQueue);
    }
    while (!local_queue.empty())
    {
        DecodedTaskHolder* holder = local_queue.front();
        local_queue.pop_front();
        RecycleDecodedTaskHolder(holder);
    }
}

void MppDecInstance::ForceRecycleAllHolders()
{
    std::deque<DecodedTaskHolder*> local_queue;
    {
        std::lock_guard<std::mutex> lock(HolderMutex);
        for (auto& kv : OutDesc2HolderMap)
        {
            local_queue.push_back(kv.second);
        }
        OutDesc2HolderMap.clear();
        while (!PendingRecycleQueue.empty())
        {
            local_queue.push_back(PendingRecycleQueue.front());
            PendingRecycleQueue.pop_front();
        }
    }
    while (!local_queue.empty())
    {
        DecodedTaskHolder* holder = local_queue.front();
        local_queue.pop_front();
        RecycleDecodedTaskHolder(holder);
    }
}

int MppDecInstance::DecQueueOutputForRecycle(const IO_FD_t* output_desc)
{
    if (!output_desc)
    {
        return -1;
    }

    std::lock_guard<std::mutex> lock(HolderMutex);
    const auto                  it = OutDesc2HolderMap.find(output_desc);
    if (it == OutDesc2HolderMap.end())
    {
        std::cerr << "Output descriptor is not in-flight, fd="
                  << ((output_desc) ? output_desc->fd : -1) << std::endl;
        return -1;
    }

    PendingRecycleQueue.push_back(it->second);
    OutDesc2HolderMap.erase(it);  // 擦除记录
    return 0;
}

int MppDecInstance::DecInit()
{
    if (mpp_ctx && mpp_api)
    {
        return 0;
    }

    MPP_RET   ret           = MPP_NOK;
    MppDecCfg dec_cfg       = nullptr;
    RK_U32    output_format = MPP_FMT_YUV420SP;

    ret = mpp_create(&mpp_ctx, &mpp_api);
    if (ret != MPP_OK || !mpp_ctx || !mpp_api)
    {
        goto fail;
    }

    ret = mpp_init(mpp_ctx, MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG);
    if (ret != MPP_OK)
    {
        goto fail;
    }

    ret = mpp_dec_cfg_init(&dec_cfg);
    if (ret != MPP_OK || !dec_cfg)
    {
        goto fail;
    }

    ret = mpp_api->control(mpp_ctx, MPP_DEC_GET_CFG, dec_cfg);
    if (ret == MPP_OK)
    {
        ret = mpp_dec_cfg_set_u32(dec_cfg, "base:split_parse", 0);
    }
    if (ret == MPP_OK)
    {
        ret = mpp_api->control(mpp_ctx, MPP_DEC_SET_CFG, dec_cfg);
    }
    mpp_dec_cfg_deinit(dec_cfg);
    dec_cfg = nullptr;

    if (ret != MPP_OK)
    {
        goto fail;
    }

    // 强制 NV12 输出，便于后续统一处理。
    ret = mpp_api->control(mpp_ctx, MPP_DEC_SET_OUTPUT_FORMAT, &output_format);
    if (ret != MPP_OK)
    {
        goto fail;
    }

    return 0;

fail:
    if (dec_cfg)
    {
        mpp_dec_cfg_deinit(dec_cfg);
        dec_cfg = nullptr;
    }
    if (group)
    {
        (void)mpp_buffer_group_clear(group);
        ReleaseExternalOutputBuffers();
        mpp_buffer_group_put(group);
        group = nullptr;
    }
    if (mpp_ctx)
    {
        mpp_destroy(mpp_ctx);
        mpp_ctx = nullptr;
        mpp_api = nullptr;
    }
    return -1;
}

int MppDecInstance::DecAllocBuffer(const FrameDesc* frame_desc_array, size_t frame_desc_count)
{
    if (!mpp_ctx || !mpp_api)
    {
        return -1;
    }
    if (!frame_desc_array || frame_desc_count == 0)
    {
        return -1;
    }
    if (group)
    {
        return 0;
    }
    // 这个函数需要在 DecConfigWidthHeight 之后进行调用。

    MPP_RET ret =
        mpp_buffer_group_get_external(&group, MPP_BUFFER_TYPE_EXT_DMA);  // 模式三：外部缓冲组
    if (ret != MPP_OK || !group)
    {
        group = nullptr;
        return -1;
    }
    ret = mpp_buffer_group_limit_config(
        group, OutSize,
        resource_limits::kMppOutputBufferCount);  // 配置上限，实际缓冲通过 commit 注入
    if (ret != MPP_OK)
    {
        mpp_buffer_group_put(group);
        group = nullptr;
        return -1;
    }
    if (CommitExternalOutputBuffers(OutSize) != 0)  // 先按先验尺寸申请并提交外部输出缓冲
    {
        mpp_buffer_group_put(group);
        group = nullptr;
        return -1;
    }
    ret = mpp_api->control(mpp_ctx, MPP_DEC_SET_EXT_BUF_GROUP, group);  // 将缓冲组关联到解码上下文
    if (ret != MPP_OK)
    {
        (void)mpp_buffer_group_clear(group);
        ReleaseExternalOutputBuffers();
        mpp_buffer_group_put(group);
        group = nullptr;
        return -1;
    }
    auto cleanup_import_resources = [&]()
    {
        for (auto& buffer : MppBuffers)
        {
            if (buffer)
            {
                mpp_buffer_put(buffer);
                buffer = nullptr;
            }
        }
        if (group)
        {
            (void)mpp_buffer_group_clear(group);
            ReleaseExternalOutputBuffers();
            mpp_buffer_group_put(group);
            group = nullptr;
        }
    };

    // 维持 V4L2 index 和 MPP index 一一对应：
    // 每个 FrameDesc 按 index 导入到对应的 MppBuffers[slot]。
    size_t       imported_count = 0;
    const size_t import_limit   = (frame_desc_count < resource_limits::kMppImportBufferCount)
                                      ? frame_desc_count
                                      : resource_limits::kMppImportBufferCount;  // 避免越界访问
    for (size_t i = 0; i < import_limit; ++i)                                    // 遍历
    {
        const FrameDesc& frame_desc = frame_desc_array[i];  // 引用当前帧描述，避免不必要的拷贝
        if (frame_desc.fd < 0 || frame_desc.Length == 0)
        {
            continue;  // 跳过无效的 fd 信息
        }

        const int slot = frame_desc.index;
        if (slot < 0 || static_cast<size_t>(slot) >= resource_limits::kMppImportBufferCount)
        {
            std::cerr << "Invalid frame index for MPP import, index=" << slot
                      << ", fd=" << frame_desc.fd << std::endl;
            cleanup_import_resources();
            return -1;
        }
        if (MppBuffers[slot])  // 同一 slot 已经有缓冲导入，说明 index 重复了，属于错误情况
        {
            std::cerr << "Duplicate frame index for MPP import, index=" << slot
                      << ", fd=" << frame_desc.fd << std::endl;
            cleanup_import_resources();
            return -1;
        }

        MppBufferInfo info{};
        info.type  = MPP_BUFFER_TYPE_EXT_DMA;  // 指定导入的缓冲类型为 dma-buf
        info.fd    = frame_desc.fd;
        info.size  = frame_desc.Length;
        info.index = slot;

        ret = mpp_buffer_import(&MppBuffers[slot], &info);  // 将 dma-buf fd 导入到 MPP 缓冲句柄中
        if (ret != MPP_OK || !MppBuffers[slot])
        {
            std::cerr << "Failed to import dma-buf fd to MPP, index=" << slot
                      << ", fd=" << frame_desc.fd << std::endl;
            cleanup_import_resources();
            return -1;
        }

        ++imported_count;
    }

    if (imported_count == 0)  // 没有成功导入任何有效的 dma-buf fd，清理资源并返回错误
    {
        cleanup_import_resources();
        std::cerr << "No valid dma-buf fd to import into MPP" << std::endl;
        return -1;
    }
    return 0;
}

int MppDecInstance::DecConfigWidthHeight(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
    {
        return -1;
    }

    ImgWidth  = width;
    ImgHeight = height;
    H_Stride  = mpp_common::Align16(ImgWidth);
    V_Stride  = mpp_common::Align16(ImgHeight);

    // 兼容 JPEG 解码常见的附加信息/对齐需求，按 2 bytes/pixel 预留更稳妥。
    OutSize = static_cast<size_t>(H_Stride) * static_cast<size_t>(V_Stride) * 2u;
    return 0;
}

int MppDecInstance::MppDecode(const FrameDesc* frame_desc)
{
    // 先回收上一轮挂起的输出任务，确保 frame/buf 生命周期由业务控制（至少延后一轮）。
    DrainPendingRecycleQueue();

    MPP_RET            ret               = MPP_NOK;
    int                outbuf_fd         = -1;
    size_t             MappedBufferIndex = 0;
    IO_FD_t*           output_desc       = nullptr;
    DecodedTaskHolder* holder            = nullptr;

    CurrentOutputDesc = nullptr;

    if (!frame_desc)
    {
        return -1;
    }

    FrameDesc*   mutable_frame_desc = const_cast<FrameDesc*>(frame_desc);
    const int    buffer_index       = frame_desc->index;
    const void*  mapped_base        = frame_desc->base;
    const size_t payload_size       = frame_desc->payloadSize;

    if (!mpp_ctx || !mpp_api || !group || !mapped_base || payload_size == 0 || OutSize == 0)
    {
        return -1;
    }
    if (buffer_index < 0 ||
        static_cast<size_t>(buffer_index) >= resource_limits::kMppImportBufferCount)
    {
        std::cerr << "Invalid input buffer index for decode: " << buffer_index << std::endl;
        return -1;
    }
    if (frame_desc->Length > 0 && payload_size > frame_desc->Length)
    {
        std::cerr << "Invalid payload size for decode, payload=" << payload_size
                  << ", capacity=" << frame_desc->Length << std::endl;
        return -1;
    }

    const auto*  input_data = static_cast<const uint8_t*>(mapped_base);  // 方便后续字节操作
    const size_t effective_size = mpp_common::FindJpegEffectiveSize(
        input_data, payload_size);  // 计算有效载荷大小，裁掉可能的对齐填充
    if (effective_size == 0)
    {
        return -1;
    }

    // camera 线程在 DQBUF 后执行了 DMA_BUF_SYNC_START(READ)；
    // CPU 读取完 JPEG 头后，需要在交给 MPP 硬件前结束 CPU 读访问窗口。
    if (mutable_frame_desc && mutable_frame_desc->cpuSyncReadActive)
    {
        if (!dma_buf_sync_end_read(mutable_frame_desc->fd))
        {
            std::cerr << "Failed to end dma-buf cpu read sync before decode, fd="
                      << mutable_frame_desc->fd << ": " << std::strerror(errno) << std::endl;
            return -1;
        }
        mutable_frame_desc->cpuSyncReadActive = false;
    }

    MppBuffer input_buffer = MppBuffers[buffer_index];  // 零拷贝路径用导入的 camera dma-buf
    if (!input_buffer)
    {
        std::cerr << "Input MppBuffer is null, index=" << buffer_index << std::endl;
        return -1;
    }
    if (effective_size > mpp_buffer_get_size(input_buffer))
    {
        std::cerr << "Effective jpeg size exceeds imported input buffer, size=" << effective_size
                  << ", buf_size=" << mpp_buffer_get_size(input_buffer)
                  << ", index=" << buffer_index << std::endl;
        return -1;
    }

    int64_t packet_pts_us = frame_desc->pts_us;
    int64_t packet_dts_us = frame_desc->dts_us;
    if (packet_pts_us >= 0)
    {
        packet_dts_us = packet_pts_us;
    }
    else if (packet_dts_us >= 0)
    {
        packet_pts_us = packet_dts_us;
    }

    bool holder_wait_logged = false;
    while (!holder)
    {
        holder =
            AcquireDecodedTaskHolder();  // 从资源池获取一个 holder 来承载本次解码任务的输出资源
        if (holder)
        {
            break;
        }

        if (!holder_wait_logged)
        {
            std::cerr << "No free decoded holder available, wait " << kHolderWaitInterval.count()
                      << "ms and retry" << std::endl;
            holder_wait_logged = true;
        }

        std::this_thread::sleep_for(kHolderWaitInterval);
        DrainPendingRecycleQueue();
    }

    MppBuffer out_buf_local           = nullptr;
    MppFrame  out_frm_local           = nullptr;
    MppFrame  decoded_frame           = nullptr;
    MppPacket packet_local            = nullptr;
    MppTask   input_task              = nullptr;
    MppTask   output_task             = nullptr;
    bool      input_task_is_submitted = false;

    auto release_local_resources = [&]()
    {
        if (out_frm_local)
        {
            mpp_frame_deinit(&out_frm_local);
            out_frm_local = nullptr;
        }
        if (out_buf_local)
        {
            mpp_buffer_put(out_buf_local);
            out_buf_local = nullptr;
        }
        if (packet_local)
        {
            mpp_packet_deinit(&packet_local);
            packet_local = nullptr;
        }
    };

    ret = mpp_buffer_get(group, &out_buf_local, OutSize);  // 从缓存池申请指定大小的内存块
    if (ret != MPP_OK || !out_buf_local)
    {
        goto fail;
    }

    ret = mpp_frame_init(&out_frm_local);
    if (ret != MPP_OK || !out_frm_local)
    {
        goto fail;
    }

    mpp_frame_set_width(out_frm_local, ImgWidth);
    mpp_frame_set_height(out_frm_local, ImgHeight);
    mpp_frame_set_hor_stride(out_frm_local, H_Stride);
    mpp_frame_set_ver_stride(out_frm_local, V_Stride);
    mpp_frame_set_fmt(out_frm_local, MPP_FMT_YUV420SP);
    mpp_frame_set_buffer(out_frm_local, out_buf_local);

    ret = mpp_packet_init_with_buffer(&packet_local, input_buffer);
    if (ret != MPP_OK || !packet_local)
    {
        goto fail;
    }

    // 在用户态进行数据解析的必要操作
    mpp_packet_set_data(
        packet_local,
        const_cast<void*>(mapped_base));  // 设置数据指针为映射后的地址，供 MPP 内部访问
    mpp_packet_set_pos(
        packet_local,
        const_cast<void*>(mapped_base));  // 设置当前位置指针为数据起始地址，供 MPP 内部访问
    mpp_packet_set_length(packet_local, effective_size);  // 设置数据长度为有效载荷大小
    mpp_packet_set_pts(packet_local, packet_pts_us);
    mpp_packet_set_dts(packet_local, packet_dts_us);

    ret = mpp_api->poll(mpp_ctx, MPP_PORT_INPUT, kPollTimeout500Ms);
    if (ret < 0)
    {
        goto fail;
    }

    ret = mpp_api->dequeue(mpp_ctx, MPP_PORT_INPUT, &input_task);
    if (ret != MPP_OK || !input_task)
    {
        goto fail;
    }

    ret = mpp_task_meta_set_packet(input_task, KEY_INPUT_PACKET, packet_local);
    if (ret != MPP_OK)
    {
        goto fail;
    }

    ret = mpp_task_meta_set_frame(input_task, KEY_OUTPUT_FRAME, out_frm_local);
    if (ret != MPP_OK)
    {
        goto fail;
    }

    ret = mpp_api->enqueue(mpp_ctx, MPP_PORT_INPUT, input_task);  // 提交解码任务
    if (ret != MPP_OK)
    {
        goto fail;
    }
    input_task_is_submitted = true;
    input_task              = nullptr;
    packet_local = nullptr;  // packet 已交给 input task，后续在回收该 task 时释放

    ret = mpp_api->poll(mpp_ctx, MPP_PORT_OUTPUT, kPollTimeout500Ms);
    if (ret < 0)
    {
        goto fail;
    }

    ret = mpp_api->dequeue(mpp_ctx, MPP_PORT_OUTPUT, &output_task);
    if (ret != MPP_OK || !output_task)
    {
        goto fail;
    }

    ret = mpp_task_meta_get_frame(output_task, KEY_OUTPUT_FRAME, &decoded_frame);
    if (ret != MPP_OK || !decoded_frame)
    {
        goto fail;
    }

    if (mpp_frame_get_info_change(decoded_frame))
    {
        {
            std::lock_guard<std::mutex> lock(HolderMutex);
            if (!OutDesc2HolderMap.empty())
            {
                std::cerr << "Info change detected while outputs are still in-flight, inflight="
                          << OutDesc2HolderMap.size() << std::endl;
                goto fail;
            }
        }
        // 模式三：info change 后重建外部输出缓冲池并重新绑定到解码器。
        // 注意：这里使用的是 frame 所需的缓冲容量，不用于像素格式判断。
        const size_t required_buf_capacity = mpp_frame_get_buf_size(decoded_frame);
        OutSize = (OutSize > required_buf_capacity) ? OutSize : required_buf_capacity;
        ret     = mpp_buffer_group_limit_config(
                group, OutSize, resource_limits::kMppOutputBufferCount);  // 更新缓冲上限配置
        if (ret != MPP_OK)
        {
            goto fail;
        }
        if (CommitExternalOutputBuffers(OutSize) != 0)
        {
            goto fail;
        }
        ret = mpp_api->control(mpp_ctx, MPP_DEC_SET_EXT_BUF_GROUP, group);  // 重新关联缓冲组
        if (ret != MPP_OK)
        {
            goto fail;
        }
        ret = mpp_api->control(mpp_ctx, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
        if (ret != MPP_OK)
        {
            goto fail;
        }
    }

    if (mpp_frame_get_errinfo(decoded_frame) || mpp_frame_get_discard(decoded_frame))
    {
        goto fail;
    }

    outbuf_fd = mpp_buffer_get_fd(
        mpp_frame_get_buffer(decoded_frame));  // 获取解码输出帧底层缓冲的 dma-buf fd
    if (outbuf_fd < 0)
    {
        goto fail;
    }
    {
        const auto out_it = OutBufFD2Index_Map.find(outbuf_fd);  // 根据映射关系找到对应的 IO_
        if (out_it == OutBufFD2Index_Map.end())
        {
            std::cerr << "Output fd is not in committed external buffer map, fd=" << outbuf_fd
                      << std::endl;
            goto fail;
        }
        MappedBufferIndex = out_it->second;  // 查找输出 fd 对应的 MPP 缓冲索引，也就是知道 Index 了
    }
    {
        if (MappedBufferIndex >= resource_limits::kMppOutputBufferCount)
        {
            std::cerr << "Mapped output index out of range: " << MappedBufferIndex << std::endl;
            goto fail;
        }
        const RK_S32 frame_width  = mpp_frame_get_width(decoded_frame);
        const RK_S32 frame_height = mpp_frame_get_height(decoded_frame);
        const RK_S32 frame_hs     = mpp_frame_get_hor_stride(decoded_frame);
        const RK_S32 frame_vs     = mpp_frame_get_ver_stride(decoded_frame);
        const RK_S32 frame_fmt =
            static_cast<RK_S32>(mpp_frame_get_fmt(decoded_frame) & MPP_FRAME_FMT_MASK);
        if (frame_width <= 0 || frame_height <= 0 || frame_hs <= 0 || frame_vs <= 0)
        {
            std::cerr << "Invalid decoded frame geometry: w=" << frame_width
                      << ", h=" << frame_height << ", hs=" << frame_hs << ", vs=" << frame_vs
                      << std::endl;
            goto fail;
        }
        if (!MPP_FRAME_FMT_IS_YUV(frame_fmt))
        {
            std::cerr << "Unsupported decoded frame format: fmt=" << frame_fmt << std::endl;
            goto fail;
        }

        IO_FD_t& output_info =
            MppOutputFDList[MappedBufferIndex];  // 根据 index 获取对应的输出描述符信息结构体
        output_info.format     = static_cast<uint32_t>(frame_fmt);
        output_info.width      = static_cast<uint32_t>(frame_width);
        output_info.height     = static_cast<uint32_t>(frame_height);
        output_info.hor_stride = static_cast<uint32_t>(frame_hs);
        output_info.ver_stride = static_cast<uint32_t>(frame_vs);
        output_info.pts_us     = mpp_frame_get_pts(decoded_frame);
        output_info.dts_us     = mpp_frame_get_dts(decoded_frame);
        if (output_info.pts_us >= 0)
        {
            output_info.dts_us = output_info.pts_us;
        }
        else if (output_info.dts_us >= 0)
        {
            output_info.pts_us = output_info.dts_us;
        }
        output_desc       = &output_info;  // 取地址
        CurrentOutputDesc = output_desc;
    }

    // 输出任务不再延迟，先归还任务通道，避免阻塞后续解码。
    ret = mpp_api->enqueue(mpp_ctx, MPP_PORT_OUTPUT, output_task);
    if (ret != MPP_OK)
    {
        goto fail;
    }
    output_task = nullptr;

    // 立即回收输入端 task 并释放 packet，保持输入端任务流畅。
    ret = mpp_api->poll(mpp_ctx, MPP_PORT_INPUT, kPollTimeout500Ms);
    if (ret < 0)
    {
        goto fail;
    }

    ret = mpp_api->dequeue(mpp_ctx, MPP_PORT_INPUT, &input_task);
    if (ret != MPP_OK || !input_task)
    {
        goto fail;
    }
    input_task_is_submitted = false;
    ReleaseTaskPacket(input_task);
    ret = mpp_api->enqueue(mpp_ctx, MPP_PORT_INPUT, input_task);
    if (ret != MPP_OK)
    {
        goto fail;
    }
    input_task = nullptr;

    holder->output_desc = output_desc;  // 记录输出描述符信息，供业务层后续回收时使用
    holder->output_frame = out_frm_local;  // 记录输出帧信息，供业务层后续访问像素数据使用
                                           // 这里记录的是指向实际对象的指针
    holder->output_buf = out_buf_local;  // 记录输出缓冲信息，供业务层后续访问底层缓冲资源使用
    {
        std::lock_guard<std::mutex> lock(HolderMutex);
        if (OutDesc2HolderMap.find(output_desc) != OutDesc2HolderMap.end())
        {
            std::cerr << "Duplicate in-flight output descriptor detected, fd="
                      << ((output_desc) ? output_desc->fd : -1) << std::endl;
            goto fail;
        }
        OutDesc2HolderMap[output_desc] =
            holder;  // 记录输出描述符到 holder 的映射关系，供后续回收时快速定位
    }

    out_frm_local = nullptr;
    out_buf_local = nullptr;
    holder        = nullptr;
    return 0;

fail:
    CurrentOutputDesc = nullptr;
    if (output_task)
    {
        (void)mpp_api->enqueue(mpp_ctx, MPP_PORT_OUTPUT, output_task);
        output_task = nullptr;
    }
    if (input_task)
    {
        ReleaseTaskPacket(input_task);
        (void)mpp_api->enqueue(mpp_ctx, MPP_PORT_INPUT, input_task);
        input_task = nullptr;
    }

    if (input_task_is_submitted)
    {
        MppTask recycle_task = nullptr;
        if (mpp_api->poll(mpp_ctx, MPP_PORT_INPUT, MPP_POLL_NON_BLOCK) >= 0 &&
            mpp_api->dequeue(mpp_ctx, MPP_PORT_INPUT, &recycle_task) == MPP_OK && recycle_task)
        {
            ReleaseTaskPacket(recycle_task);
            (void)mpp_api->enqueue(mpp_ctx, MPP_PORT_INPUT, recycle_task);
        }
    }

    release_local_resources();
    ReturnDecodedTaskHolder(holder);
    holder = nullptr;

    return -1;
}

MppDecInstance::~MppDecInstance()
{
    ForceRecycleAllHolders();
    DrainPendingRecycleQueue();

    for (auto& buffer : MppBuffers)
    {
        if (buffer)
        {
            mpp_buffer_put(buffer);
            buffer = nullptr;
        }
    }

    if (group)
    {
        (void)mpp_buffer_group_clear(group);
        ReleaseExternalOutputBuffers();
        mpp_buffer_group_put(group);
        group = nullptr;
    }
    if (mpp_ctx)
    {
        mpp_destroy(mpp_ctx);
        mpp_ctx = nullptr;
        mpp_api = nullptr;
    }
}

int MppDecInstance::AllocDmaBufFD(IO_FD_t& output, size_t size)
{
    if (size == 0)
    {
        std::cerr << "Invalid size for DMA buffer allocation: " << size << std::endl;
        return -1;
    }
    int heap = open(mpp_common::kSystemDmaHeapPath, O_RDWR | O_CLOEXEC);
    if (heap < 0)
    {
        perror("Failed to open dma_heap");
        return -1;
    }

    dma_heap_allocation_data req{};
    req.len      = size;
    req.fd_flags = O_RDWR | O_CLOEXEC;

    if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &req) < 0)
    {
        perror("DMA heap allocation failed");
        close(heap);
        return -1;
    }

    close(heap);
    heap = -1;

    void* mapped_base = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, req.fd, 0);
    if (mapped_base == MAP_FAILED)
    {
        std::cerr << "Failed to mmap output dma-buf fd=" << req.fd << ": " << std::strerror(errno)
                  << std::endl;
        close(req.fd);
        return -1;
    }

    output.fd         = req.fd;
    output.base       = mapped_base;
    output.size       = size;
    output.format     = 0;
    output.width      = 0;
    output.height     = 0;
    output.hor_stride = 0;
    output.ver_stride = 0;
    output.pts_us     = -1;
    output.dts_us     = -1;
    return 0;
}

void MppDecInstance::ReleaseExternalOutputBuffers()
{
    // 外部输出缓冲重建或析构前，先把挂起的输出 task/frame/buf 全部归还。
    ForceRecycleAllHolders();
    DrainPendingRecycleQueue();

    for (auto& output : MppOutputFDList)
    {
        if (output.base && output.size > 0)
        {
            if (munmap(output.base, output.size) != 0)
            {
                std::cerr << "Failed to munmap output dma-buf fd=" << output.fd << ": "
                          << std::strerror(errno) << std::endl;
            }
            output.base = nullptr;
            output.size = 0;
        }
        if (output.fd >= 0)
        {
            if (close(output.fd) != 0)
            {
                std::cerr << "Failed to close output dma-buf fd=" << output.fd << ": "
                          << std::strerror(errno) << std::endl;
            }
            output.fd = -1;
        }
        output.format     = 0;
        output.width      = 0;
        output.height     = 0;
        output.hor_stride = 0;
        output.ver_stride = 0;
        output.pts_us     = -1;
        output.dts_us     = -1;
    }
    OutBufFD2Index_Map.clear();  // 清空 fd 到输出缓冲索引的映射
    CurrentOutputDesc = nullptr;
}

int MppDecInstance::CommitExternalOutputBuffers(size_t size)
{
    if (!group || size == 0)
    {
        return -1;
    }

    {
        std::lock_guard<std::mutex> lock(HolderMutex);
        if (!OutDesc2HolderMap.empty())
        {
            std::cerr << "Can not commit external buffers while outputs are in-flight, inflight="
                      << OutDesc2HolderMap.size() << std::endl;
            return -1;
        }
    }

    DrainPendingRecycleQueue();

    // 先清理组里旧的可复用缓冲，再释放本地记录的 fd。
    MPP_RET ret = mpp_buffer_group_clear(group);
    if (ret != MPP_OK)
    {
        return -1;
    }
    ReleaseExternalOutputBuffers();

    MppBufferInfo commit{};
    commit.type = MPP_BUFFER_TYPE_EXT_DMA;
    commit.size = size;

    for (size_t i = 0; i < resource_limits::kMppOutputBufferCount; ++i)
    {
        IO_FD_t& output = MppOutputFDList[i];
        if (AllocDmaBufFD(output, size) != 0)
        {
            (void)mpp_buffer_group_clear(group);
            ReleaseExternalOutputBuffers();
            return -1;
        }
        OutBufFD2Index_Map[output.fd] =
            i;  // 记录成功提交的输出缓冲 fd 与索引映射，便于后续按 fd 反查索引
        commit.fd = output.fd;  // 提交外部 dma-buf fd 给 MPP 在 group 生命周期内使用
        commit.ptr =
            nullptr;  // MPP_BUFFER_TYPE_EXT_DMA 模式下 ptr 不需要设置，确保为 nullptr 以免误用
        commit.index = static_cast<int>(i);                // 用于跟踪
        ret          = mpp_buffer_commit(group, &commit);  // 提交缓冲到组
        if (ret != MPP_OK)
        {
            std::cerr << "Failed to commit external output buffer, index=" << i
                      << ", fd=" << output.fd << std::endl;
            (void)mpp_buffer_group_clear(group);
            ReleaseExternalOutputBuffers();
            return -1;
        }
    }

    return 0;
}
