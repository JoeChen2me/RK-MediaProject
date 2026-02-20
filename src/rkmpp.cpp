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

int MppInstance::MppAllocBuffer(const v4l2FD_Info* dma_fd_array, size_t dma_fd_count)
{
    if (!mpp_ctx || !mpp_api)
    {
        return -1;
    }
    if (!dma_fd_array || dma_fd_count == 0)
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

    // 维持 V4L2 index 和 MPP index 一一对应：
    // 第 i 个 V4L2 fd 固定导入到 MppBuffers[i]，不做压缩重排。
    size_t       imported_count = 0;
    const size_t import_limit =
        (dma_fd_count < kMaxImportBuffers) ? dma_fd_count : kMaxImportBuffers;  // 避免越界访问
    for (size_t i = 0; i < import_limit; ++i)
    {
        const v4l2FD_Info& fd_info = dma_fd_array[i];  // 引用传递，避免不必要的复制
        if (fd_info.fd < 0 || fd_info.bufferSize == 0)
        {
            continue;  // 跳过无效的 fd 信息
        }

        MppBufferInfo info{};
        info.type  = MPP_BUFFER_TYPE_EXT_DMA;  // 指定导入的缓冲类型为 dma-buf
        info.fd    = fd_info.fd;               // 直接使用传入的文件描述符
        info.size  = fd_info.bufferSize;       // 使用传入的缓冲区大小
        info.index = static_cast<int>(i);

        ret = mpp_buffer_import(&MppBuffers[i],
                                &info);  // 将 dma-buf fd 导入到 MPP 缓冲句柄中
        if (ret != MPP_OK || !MppBuffers[i])
        {
            std::cerr << "Failed to import dma-buf fd to MPP, index=" << i << ", fd=" << fd_info.fd
                      << std::endl;
            for (size_t j = 0; j < import_limit; ++j)
            {
                if (MppBuffers[j])
                {
                    mpp_buffer_put(MppBuffers[j]);
                    MppBuffers[j] = nullptr;
                }
            }
            mpp_buffer_group_put(group);
            group = nullptr;
            return -1;
        }

        ++imported_count;
    }

    if (imported_count == 0)
    {
        mpp_buffer_group_put(group);
        group = nullptr;
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

int MppInstance::MppDecode(const uint8_t* data, size_t len)
{
    MPP_RET ret            = MPP_NOK;
    size_t  effective_size = 0;
    bool    taskFromOutput = false;

    if (!mpp_ctx || !mpp_api || !group || !data || len == 0 || OutSize == 0)
    {
        return -1;
    }

    effective_size = FindJpegEffectiveSize(data, len);
    if (effective_size == 0)
    {
        return -1;
    }

    MppBuffer out_buf_local = nullptr;
    MppBuffer pkt_buf_local = nullptr;
    MppFrame  out_frm_local = nullptr;
    MppPacket packet_local  = nullptr;
    MppTask   task_local    = nullptr;
    MppFrame  decoded_frame = nullptr;

    ret = mpp_buffer_get(group, &out_buf_local, OutSize);  // 从缓冲组获取一个输出缓冲
    if (ret != MPP_OK || !out_buf_local)
    {
        goto fail;
    }

    ret = mpp_frame_init(&out_frm_local);
    if (ret != MPP_OK || !out_frm_local)
    {
        goto fail;
    }

    // 对于 JPEG task 模式，主要由解码器写入输出 frame 信息，这里仅绑定用户输出缓冲。
    mpp_frame_set_width(out_frm_local, ImgWidth);
    mpp_frame_set_height(out_frm_local, ImgHeight);
    mpp_frame_set_hor_stride(out_frm_local, H_Stride);
    mpp_frame_set_ver_stride(out_frm_local, V_Stride);
    mpp_frame_set_fmt(out_frm_local, MPP_FMT_YUV420SP);

    mpp_frame_set_buffer(out_frm_local, out_buf_local);

    ret = mpp_buffer_get(group, &pkt_buf_local, effective_size);  // 从缓冲组获取一个输入缓冲
    if (ret != MPP_OK || !pkt_buf_local)
    {
        goto fail;
    }

    ret = mpp_buffer_write(pkt_buf_local, 0, const_cast<uint8_t*>(data), effective_size);
    if (ret != MPP_OK)
    {
        goto fail;
    }

    ret = mpp_packet_init_with_buffer(&packet_local, pkt_buf_local);
    if (ret != MPP_OK || !packet_local)
    {
        goto fail;
    }
    mpp_packet_set_pos(packet_local, mpp_buffer_get_ptr(pkt_buf_local));
    mpp_packet_set_length(packet_local, effective_size);

    ret = mpp_api->poll(mpp_ctx, MPP_PORT_INPUT, kPollTimeout500Ms);
    if (ret < 0)
    {
        goto fail;
    }

    ret = mpp_api->dequeue(mpp_ctx, MPP_PORT_INPUT, &task_local);
    if (ret != MPP_OK || !task_local)
    {
        goto fail;
    }

    ret = mpp_task_meta_set_packet(task_local, KEY_INPUT_PACKET, packet_local);
    if (ret != MPP_OK)
    {
        goto fail;
    }

    ret = mpp_task_meta_set_frame(task_local, KEY_OUTPUT_FRAME, out_frm_local);
    if (ret != MPP_OK)
    {
        goto fail;
    }

    ret = mpp_api->enqueue(mpp_ctx, MPP_PORT_INPUT, task_local);
    if (ret != MPP_OK)
    {
        goto fail;
    }
    task_local = nullptr;

    ret = mpp_api->poll(mpp_ctx, MPP_PORT_OUTPUT, kPollTimeout500Ms);
    if (ret < 0)
    {
        goto fail;
    }

    ret = mpp_api->dequeue(mpp_ctx, MPP_PORT_OUTPUT, &task_local);
    if (ret != MPP_OK || !task_local)
    {
        goto fail;
    }
    taskFromOutput = true;

    ret = mpp_task_meta_get_frame(task_local, KEY_OUTPUT_FRAME, &decoded_frame);
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
    // 打印输出帧的信息
    // std::cout << "Decoded frame info: width=" << mpp_frame_get_width(decoded_frame)
    //           << ", height=" << mpp_frame_get_height(decoded_frame)
    //           << ", hor_stride=" << mpp_frame_get_hor_stride(decoded_frame)
    //           << ", ver_stride=" << mpp_frame_get_ver_stride(decoded_frame)
    //           << ", fmt=" << mpp_frame_get_fmt(decoded_frame) << std::endl;
    ret = mpp_api->enqueue(mpp_ctx, MPP_PORT_OUTPUT, task_local);
    if (ret != MPP_OK)
    {
        goto fail;
    }
    task_local = nullptr;

    if (packet_local)
    {
        mpp_packet_deinit(&packet_local);
        packet_local = nullptr;
    }
    if (pkt_buf_local)
    {
        mpp_buffer_put(pkt_buf_local);
        pkt_buf_local = nullptr;
    }
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
    return 0;

fail:
    if (task_local && mpp_ctx && mpp_api)
    {
        mpp_api->enqueue(mpp_ctx, taskFromOutput ? MPP_PORT_OUTPUT : MPP_PORT_INPUT, task_local);
        task_local = nullptr;
    }
    if (packet_local)
    {
        mpp_packet_deinit(&packet_local);
        packet_local = nullptr;
    }
    if (pkt_buf_local)
    {
        mpp_buffer_put(pkt_buf_local);
        pkt_buf_local = nullptr;
    }
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
