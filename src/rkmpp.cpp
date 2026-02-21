#include "rkmpp.h"
#include <iostream>
namespace
{
constexpr MppPollType kPollTimeout500Ms = static_cast<MppPollType>(500);

inline uint32_t Align16(uint32_t value) { return (value + 15u) & ~15u; }

// 从帧尾部回扫 EOI(FFD9)，裁掉 V4L2 可能附带的对齐填充。
size_t FindJpegEffectiveSize(const uint8_t* data, size_t len)
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
}  // namespace

MppInstance::MppInstance() = default;

int MppInstance::MppInit()
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

int MppInstance::MppAllocBuffer(const FrameDesc* frame_desc_array, size_t frame_desc_count)
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

    MPP_RET ret = mpp_buffer_group_get_internal(&group, MPP_BUFFER_TYPE_ION);
    if (ret != MPP_OK || !group)
    {
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
            mpp_buffer_group_put(group);
            group = nullptr;
        }
    };

    // 维持 V4L2 index 和 MPP index 一一对应：
    // 每个 FrameDesc 按 index 导入到对应的 MppBuffers[slot]。
    size_t       imported_count = 0;
    const size_t import_limit   = (frame_desc_count < kMaxImportBuffers)
                                      ? frame_desc_count
                                      : kMaxImportBuffers;  // 避免越界访问
    for (size_t i = 0; i < import_limit; ++i)               // 遍历
    {
        const FrameDesc& frame_desc = frame_desc_array[i];  // 引用当前帧描述，避免不必要的拷贝
        if (frame_desc.fd < 0 || frame_desc.Length == 0)
        {
            continue;  // 跳过无效的 fd 信息
        }

        const int slot = frame_desc.index;
        if (slot < 0 || static_cast<size_t>(slot) >= kMaxImportBuffers)
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

int MppInstance::MppConfigWidthHeight(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
    {
        return -1;
    }

    ImgWidth  = width;
    ImgHeight = height;
    H_Stride  = Align16(ImgWidth);
    V_Stride  = Align16(ImgHeight);

    // 兼容 JPEG 解码常见的附加信息/对齐需求，按 2 bytes/pixel 预留更稳妥。
    OutSize = static_cast<size_t>(H_Stride) * static_cast<size_t>(V_Stride) * 2u;
    return 0;
}

int MppInstance::MppDecode(const FrameDesc* frame_desc)
{
    MPP_RET ret = MPP_NOK;

    if (!frame_desc)
    {
        return -1;
    }

    const int    buffer_index = frame_desc->index;
    const void*  mapped_base  = frame_desc->base;
    const size_t payload_size = frame_desc->payloadSize;

    if (!mpp_ctx || !mpp_api || !group || !mapped_base || payload_size == 0 || OutSize == 0)
    {
        return -1;
    }
    if (buffer_index < 0 || static_cast<size_t>(buffer_index) >= kMaxImportBuffers)
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

    MppBuffer input_buffer = MppBuffers[buffer_index];  // 拷贝一个指针
    if (!input_buffer)
    {
        std::cerr << "Input MppBuffer is null, index=" << buffer_index << std::endl;
        return -1;
    }

    const auto*  input_data = static_cast<const uint8_t*>(mapped_base);  // 方便后续字节操作
    const size_t effective_size =
        FindJpegEffectiveSize(input_data, payload_size);  // 计算有效载荷大小，裁掉可能的对齐填充
    if (effective_size == 0 || effective_size > mpp_buffer_get_size(input_buffer))
    {
        return -1;
    }

    MppBuffer out_buf_local           = nullptr;
    MppFrame  out_frm_local           = nullptr;
    MppFrame  decoded_frame           = nullptr;
    MppPacket packet_local            = nullptr;
    MppTask   input_task              = nullptr;
    MppTask   output_task             = nullptr;
    bool      input_task_is_submitted = false;

    auto release_task_packet = [&](MppTask task)
    {
        MppPacket packet_from_task = nullptr;
        if (mpp_task_meta_get_packet(task, KEY_INPUT_PACKET, &packet_from_task) == MPP_OK &&
            packet_from_task)
        {
            if (packet_from_task == packet_local)
            {
                packet_local = nullptr;
            }
            mpp_packet_deinit(&packet_from_task);
        }
    };

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

    ret = mpp_buffer_get(group, &out_buf_local, OutSize);
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

    ret = mpp_packet_init_with_buffer(
        &packet_local,
        input_buffer);  // 将 buffer_index 对应的 MppBuffer 包装成 MppPacket 以供解码输入
    if (ret != MPP_OK || !packet_local)
    {
        goto fail;
    }
    // 在用户态进行数据解析的必要操作
    mpp_packet_set_data(packet_local,
                        const_cast<void*>(mapped_base));  // 设置数据指针为映射后的基地址
    mpp_packet_set_pos(packet_local,
                       const_cast<void*>(mapped_base));   // 设置当前位置为映射后的基地址
    mpp_packet_set_length(packet_local, effective_size);  // 设置数据长度为有效载荷大小

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

    ret = mpp_api->enqueue(mpp_ctx, MPP_PORT_OUTPUT, output_task);  // 归还输出任务
    if (ret != MPP_OK)
    {
        goto fail;
    }
    output_task = nullptr;

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

    release_task_packet(input_task);

    ret = mpp_api->enqueue(mpp_ctx, MPP_PORT_INPUT, input_task);
    if (ret != MPP_OK)
    {
        goto fail;
    }
    input_task_is_submitted = false;
    input_task              = nullptr;

    release_local_resources();
    return 0;

fail:
    if (output_task)
    {
        (void)mpp_api->enqueue(mpp_ctx, MPP_PORT_OUTPUT, output_task);
        output_task = nullptr;
    }
    if (input_task)
    {
        release_task_packet(input_task);
        (void)mpp_api->enqueue(mpp_ctx, MPP_PORT_INPUT, input_task);
        input_task = nullptr;
    }

    if (input_task_is_submitted)
    {
        MppTask recycle_task = nullptr;
        if (mpp_api->poll(mpp_ctx, MPP_PORT_INPUT, MPP_POLL_NON_BLOCK) >= 0 &&
            mpp_api->dequeue(mpp_ctx, MPP_PORT_INPUT, &recycle_task) == MPP_OK && recycle_task)
        {
            release_task_packet(recycle_task);
            (void)mpp_api->enqueue(mpp_ctx, MPP_PORT_INPUT, recycle_task);
        }
    }

    release_local_resources();

    return -1;
}

MppInstance::~MppInstance()
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
