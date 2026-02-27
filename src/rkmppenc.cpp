#include "rkmppenc.h"

#include <rockchip/mpp_err.h>

namespace
{
constexpr RK_S64      kMppTimeoutMs   = 500;
constexpr MppPollType kMppPollTimeout = static_cast<MppPollType>(500);

void ResetPacketView(EncPacketView* pkt)
{
    if (!pkt)
    {
        return;
    }
    pkt->data     = nullptr;
    pkt->len      = 0;
    pkt->dts_ms   = -1;
    pkt->pts_ms   = -1;
    pkt->eos      = false;
    pkt->is_extra = false;
    pkt->handle   = nullptr;
}
}  // namespace

MppEncInstance::MppEncInstance()
{
    for (auto& buf : MapBufferFromFD)
    {
        buf = nullptr;
    }
    ImportedFdMap_.clear();
}

MppEncInstance::~MppEncInstance()
{
    if (this->HeaderBuf_)
    {
        mpp_buffer_put(this->HeaderBuf_);
        this->HeaderBuf_ = nullptr;
    }
    if (this->HeaderBufGroup_)
    {
        mpp_buffer_group_put(this->HeaderBufGroup_);
        this->HeaderBufGroup_ = nullptr;
    }
    HeaderBufSize_ = 0;
    if (this->enc_cfg_)
    {
        mpp_enc_cfg_deinit(this->enc_cfg_);
        this->enc_cfg_ = nullptr;
    }
    for (auto& buf : MapBufferFromFD)
    {
        if (buf)
        {
            mpp_buffer_put(buf);  // 释放 MppBuffer 资源
            buf = nullptr;
        }
    }
    ImportedFdMap_.clear();    // 清理映射关系
    AllocBufferDone_ = false;  // 重置状态，防止误用
    if (this->group_)
    {
        mpp_buffer_group_clear(this->group_);  // 清理缓冲池中的所有缓冲，确保没有缓冲被占用
        mpp_buffer_group_put(this->group_);  // 释放缓冲池资源
        this->group_ = nullptr;
    }
    if (this->mpp_ctx_)
    {
        mpp_destroy(this->mpp_ctx_);
        this->mpp_ctx_ = nullptr;
        this->mpp_api_ = nullptr;
    }
}

int MppEncInstance::EncInit(MppCodingType EncodingType)
{
    if (mpp_ctx_ && mpp_api_)
    {
        return 0;  // 已初始化
    }
    RK_S64  timeout_ms = kMppTimeoutMs;
    MPP_RET ret        = mpp_create(&mpp_ctx_, &mpp_api_);
    if (ret != MPP_OK || !mpp_ctx_ || !mpp_api_)
    {
        goto fail;
    }

    EncodingType_ = EncodingType;
    ret = mpp_init(mpp_ctx_, MPP_CTX_ENC, EncodingType_);  // 编码器类型 指定编码格式
    if (ret != MPP_OK)
    {
        goto fail;
    }

    ret = mpp_enc_cfg_init(&enc_cfg_);
    if (ret != MPP_OK || !enc_cfg_)
    {
        goto fail;
    }
    ret = mpp_api_->control(mpp_ctx_, MPP_ENC_GET_CFG, enc_cfg_);
    if (ret != MPP_OK)
    {
        goto fail;
    }
    // 设置输入输出超时时间
    ret = mpp_api_->control(mpp_ctx_, MPP_SET_INPUT_TIMEOUT, &timeout_ms);
    if (ret != MPP_OK)
    {
        goto fail;
    }
    ret = mpp_api_->control(mpp_ctx_, MPP_SET_OUTPUT_TIMEOUT, &timeout_ms);
    if (ret != MPP_OK)
    {
        goto fail;
    }

    ReachPacketEOS_ = false;
    InputFrameId    = 0;

    return 0;
fail:
    if (enc_cfg_)
    {
        mpp_enc_cfg_deinit(enc_cfg_);
        enc_cfg_ = nullptr;
    }
    if (mpp_ctx_)
    {
        mpp_destroy(mpp_ctx_);
        mpp_ctx_ = nullptr;
        mpp_api_ = nullptr;
    }
    return -1;
}

int MppEncInstance::EncConfigWidthHeight(uint32_t width, uint32_t height, uint32_t hor_stride,
                                         uint32_t ver_stride, uint32_t fps, uint32_t bitrate_bps,
                                         uint32_t gop)
{
    // 校验为非空指针
    if (!mpp_ctx_ || !mpp_api_ || !enc_cfg_)
    {
        return -1;
    }
    // 校验参数有效性
    if (width == 0 || height == 0 || fps == 0 || bitrate_bps == 0 || gop == 0)
    {
        std::cerr << "Invalid parameters for encoder configuration: width=" << width
                  << ", height=" << height << ", fps=" << fps << ", bitrate_bps=" << bitrate_bps
                  << ", gop=" << gop << std::endl;
        return -1;
    }
    MPP_RET ret = MPP_NOK;
    ImgHeight_  = height;
    ImgWidth_   = width;
    H_Stride_   = hor_stride > 0 ? hor_stride
                                 : mpp_common::Align16(ImgWidth_);  // 这个值设置为 0 触发自动设置
    V_Stride_   = ver_stride > 0 ? ver_stride
                                 : mpp_common::Align16(ImgHeight_);  // 这个值设置为 0 触发自动设置
    Fps_        = (fps > 0 ? fps : 30);                              // 默认 30 fps
    BitrateBps_ =
        bitrate_bps > 0
            ? bitrate_bps
            : ImgHeight_ * ImgWidth_ * Fps_ *
                  0.125;  // 这里的 0.125 是一个经验值，实际应用中可能需要根据内容复杂度调整
    Gop_ = (gop + Fps_ - 1) &
           ~(Fps_ - 1);  // 将 gop 转换为以帧率为基数的整数倍，确保 GOP 间隔与帧率对齐
    // 参考官方示例：输出 packet 缓冲按单帧上界预估，YUV420SP 采用 1.5 倍像素字节数
    const size_t aligned_hor_stride = static_cast<size_t>((H_Stride_ + 63u) & ~63u);
    const size_t aligned_ver_stride =
        static_cast<size_t>((V_Stride_ + 63u) & ~63u);  // 相当于向上取整到 64 的倍数
    PacketBufSize_ = aligned_hor_stride * aligned_ver_stride * 3u / 2u;

    if (HeaderBuf_ && HeaderBufSize_ < PacketBufSize_)
    {
        mpp_buffer_put(HeaderBuf_);
        HeaderBuf_     = nullptr;
        HeaderBufSize_ = 0;
    }
    if (!HeaderBufGroup_)
    {
        ret = mpp_buffer_group_get_internal(&HeaderBufGroup_, MPP_BUFFER_TYPE_DRM);
        if (ret != MPP_OK || !HeaderBufGroup_)
        {
            return -1;
        }
    }
    if (!HeaderBuf_)
    {
        size_t header_size = PacketBufSize_ > 0 ? PacketBufSize_ : static_cast<size_t>(1 << 20);
        ret                = mpp_buffer_get(HeaderBufGroup_, &HeaderBuf_, header_size);
        if (ret != MPP_OK || !HeaderBuf_)
        {
            return -1;
        }
        HeaderBufSize_ = header_size;
    }
    /**
     * 三种需要配置的信息
     * 码率控制配置 RcCfg
     * 输入控制 PrepCfg
     * 协议控制配置 CodecCfg
     */
    // 码率控制配置
    mpp_enc_cfg_set_s32(enc_cfg_, "rc:mode", MPP_ENC_RC_MODE_CBR);  // 固定码率
    mpp_enc_cfg_set_s32(enc_cfg_, "rc:fps_in_flex", 0);             // 固定帧率
    mpp_enc_cfg_set_s32(enc_cfg_, "rc:fps_in_num", static_cast<RK_S32>(Fps_));
    mpp_enc_cfg_set_s32(enc_cfg_, "rc:fps_in_denorm", 1);
    mpp_enc_cfg_set_s32(enc_cfg_, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(enc_cfg_, "rc:fps_out_num", static_cast<RK_S32>(Fps_));
    mpp_enc_cfg_set_s32(enc_cfg_, "rc:fps_out_denorm", 1);
    mpp_enc_cfg_set_s32(enc_cfg_, "rc:gop", static_cast<RK_S32>(Gop_));
    mpp_enc_cfg_set_s32(enc_cfg_, "rc:bps_target",
                        static_cast<RK_S32>(BitrateBps_));  // CBR 模式下的目标码率

    // 配置输入控制
    mpp_enc_cfg_set_s32(enc_cfg_, "prep:width", static_cast<RK_S32>(ImgWidth_));
    mpp_enc_cfg_set_s32(enc_cfg_, "prep:height", static_cast<RK_S32>(ImgHeight_));
    mpp_enc_cfg_set_s32(enc_cfg_, "prep:hor_stride", static_cast<RK_S32>(H_Stride_));
    mpp_enc_cfg_set_s32(enc_cfg_, "prep:ver_stride", static_cast<RK_S32>(V_Stride_));
    mpp_enc_cfg_set_s32(enc_cfg_, "prep:format", MPP_FMT_YUV420SP);

    // 协议控制配置
    mpp_enc_cfg_set_s32(enc_cfg_, "codec:type", EncodingType_);
    if (EncodingType_ == MPP_VIDEO_CodingAVC)
    {
        // 针对 H.264 编码的特定配置
        mpp_enc_cfg_set_s32(enc_cfg_, "h264:profile", 100);  // 最高配置档
        mpp_enc_cfg_set_s32(enc_cfg_, "h264:level",
                            40);  // 限制最大性能参数，40支持最高 1920×1080 @ 30fps
        mpp_enc_cfg_set_s32(enc_cfg_, "h264:cabac_en", 1);   // 使能 CABAC
        mpp_enc_cfg_set_s32(enc_cfg_, "h264:cabac_idc", 0);  // 配置CABAC 上下文模型
        mpp_enc_cfg_set_s32(enc_cfg_, "h264:trans8x8",
                            1);  // High Profile 的标志性功能，用于提高细节保留能力
    }
    // 显式关闭 slice 分片，尽量保证一帧对应一个输出包。
    mpp_enc_cfg_set_s32(enc_cfg_, "split:mode", MPP_ENC_SPLIT_NONE);
    mpp_enc_cfg_set_s32(enc_cfg_, "split:arg", 0);
    mpp_enc_cfg_set_s32(enc_cfg_, "split:out", 0);
    // 写入配置到编码器
    ret = mpp_api_->control(mpp_ctx_, MPP_ENC_SET_CFG, enc_cfg_);
    if (ret != MPP_OK)
    {
        return -1;
    }

    // 对于 H264/H265 设置每个 IDR 帧都带 SPS/PPS，确保解码器能够正确解析关键帧
    if (EncodingType_ == MPP_VIDEO_CodingAVC || EncodingType_ == MPP_VIDEO_CodingHEVC)
    {
        MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
        ret = mpp_api_->control(mpp_ctx_, MPP_ENC_SET_HEADER_MODE, &header_mode);
        if (ret != MPP_OK)
        {
            return -1;
        }
    }
    return 0;
}

int MppEncInstance::EncoderImportBufferFromFD(const IO_FD_t* FD_Array, size_t count)
{
    std::vector<size_t> imported_indices;
    std::vector<int>    imported_fds;
    auto                cleanup_import_resources = [&]()
    {
        for (size_t index : imported_indices)
        {
            if (index >= resource_limits::kMppImportBufferCount)
            {
                continue;
            }
            if (MapBufferFromFD[index])
            {
                mpp_buffer_put(MapBufferFromFD[index]);
                MapBufferFromFD[index] = nullptr;
            }
        }
        for (int fd : imported_fds)
        {
            ImportedFdMap_.erase(fd);
        }
        imported_indices.clear();
        imported_fds.clear();
    };
    // 检查上下文指针情况
    if (!mpp_ctx_ || !mpp_api_)
    {
        return -1;
    }
    // 检查输入参数有效性
    if (!FD_Array || count == 0 || count > resource_limits::kMppImportBufferCount)
    {
        return -1;
    }
    MPP_RET ret            = MPP_NOK;
    size_t  imported_count = 0;
    // 遍历输入的 fd 数组，尝试导入每个有效的 dma-buf fd 到 MPP，并记录映射关系
    for (size_t i = 0; i < count; ++i)
    {
        if (MapBufferFromFD[i] != nullptr)
        {
            std::cerr << "Buffer for index " << i
                      << " has already been imported, fd=" << FD_Array[i].fd << std::endl;
            cleanup_import_resources();  // 清理已导入的资源
            return -1;
        }
        // 导入每个 fd 并存储对应的 MppBuffer 句柄
        MppBufferInfo info;
        info.type  = MPP_BUFFER_TYPE_DMA_HEAP;
        info.fd    = FD_Array[i].fd;
        info.size  = FD_Array[i].size;
        info.index = static_cast<int>(i);                     // 记录索引，便于后续管理
        ret = mpp_buffer_import(&MapBufferFromFD[i], &info);  // 导入 fd 到 MPP 获取 MppBuffer 句柄
        if (ret != MPP_OK || !MapBufferFromFD[i])
        {
            std::cerr << "Failed to import fd into MPP, index=" << i << ", fd=" << FD_Array[i].fd
                      << std::endl;
            cleanup_import_resources();  // 清理已导入的资源
            return -1;
        }
        // 建立映射，方便后续进行回溯
        ImportedFdMap_[FD_Array[i].fd] = MapBufferFromFD[i];
        imported_indices.push_back(i);
        imported_fds.push_back(FD_Array[i].fd);
        imported_count++;  // 统计成功导入的 fd 数量
    }
    if (imported_count == 0)
    {
        std::cerr << "No valid dma-buf fd was imported into MPP" << std::endl;
        return -1;
    }
    return 0;
}
int MppEncInstance::BuildInputFrameFromFd(const IO_FD_t* input_desc, MppFrame* out_frame,
                                          bool isEos)
{
    if (!input_desc || !out_frame)
    {
        return -1;  //
    }
    if (!mpp_api_ || !mpp_ctx_)
    {
        return -1;  // 上下文未初始化
    }
    if (input_desc->fd < 0 || input_desc->size == 0 || input_desc->width == 0 ||
        input_desc->height == 0 || input_desc->hor_stride == 0 || input_desc->ver_stride == 0)
    {
        return -1;  // 输入描述无效
    }
    // 先根据 FD 拿到对应的 Buffer
    const auto inBuf = ImportedFdMap_.find(input_desc->fd);
    if (inBuf == ImportedFdMap_.end() || !inBuf->second)
    {
        std::cerr << "Input fd not found in imported buffer map, fd=" << input_desc->fd
                  << std::endl;
        return -1;  // 输入 fd 没有成功导入到 MPP 中
    }
    MPP_RET ret = mpp_frame_init(out_frame);
    if (ret != MPP_OK || !*out_frame)
    {
        return -1;  // 创建 MppFrame 失败
    }
    mpp_frame_set_width(*out_frame, static_cast<RK_S32>(ImgWidth_));
    mpp_frame_set_height(*out_frame, static_cast<RK_S32>(ImgHeight_));
    mpp_frame_set_hor_stride(*out_frame, static_cast<RK_S32>(H_Stride_));
    mpp_frame_set_ver_stride(*out_frame, static_cast<RK_S32>(V_Stride_));
    mpp_frame_set_fmt(*out_frame, MPP_FMT_YUV420SP);  // 设置输入格式，需与编码器配置的格式一致
    mpp_frame_set_buffer(*out_frame, inBuf->second);  // 将对应的 MppBuffer 句柄关联到 MppFrame 上
    const RK_S64 frame_id = static_cast<RK_S64>(InputFrameId++);
    const RK_S64 fps      = static_cast<RK_S64>(Fps_ > 0 ? Fps_ : kDefaultFps);
    mpp_frame_set_pts(*out_frame, frame_id * 1000 / fps);  // 使用毫秒时间戳
    if (isEos)
    {
        mpp_frame_set_eos(*out_frame, 1);  // 设置 EOS 标志
    }
    return 0;
}
int MppEncInstance::AllocBufferForIO(const IO_FD_t* FD_Array, size_t count)
{
    if (!FD_Array || count == 0 || count > resource_limits::kMppImportBufferCount)
    {
        return -1;  // 输入参数无效
    }
    if (!mpp_api_ || !mpp_ctx_)
    {
        return -1;  // 上下文未初始化
    }
    MPP_RET ret = MPP_NOK;
    // 申请 DRM 内存池。当前平台库在 group_get 阶段传 flags 存在兼容性问题，先使用基础类型。
    ret = mpp_buffer_group_get_internal(&group_, MPP_BUFFER_TYPE_DRM);
    if (ret != MPP_OK)
    {
        return -1;  // 获取内部缓冲区失败
    }

    int ImportRet =
        EncoderImportBufferFromFD(FD_Array, count);  // 导入 fd 到 MPP 获取 MppBuffer 句柄
    if (ImportRet != 0)
    {
        mpp_buffer_group_put(group_);  // 释放内存池
        group_ = nullptr;
        return -1;  // 导入 fd 失败
    }
    AllocBufferDone_ = true;  // 标记已完成输入缓冲的分配和导入
    return 0;
}
int MppEncInstance::EncodePushFrame(const IO_FD_t* input_desc)
{
    if (!input_desc)
    {
        return -1;
    }
    if (mpp_api_ == nullptr || mpp_ctx_ == nullptr)
    {
        return -1;
    }
    if (!AllocBufferDone_)
    {
        std::cerr << "Input buffers have not been allocated and imported yet" << std::endl;
        return -1;  // 输入缓冲未准备好
    }
    MPP_RET  ret         = MPP_NOK;
    MppFrame input_frame = nullptr;
    int      BuildFrameRet =
        BuildInputFrameFromFd(input_desc, &input_frame);  // 构建输入帧并与 Buffer 进行绑定
    if (BuildFrameRet != 0 || !input_frame)
    {
        return -1;
    }
    ret =
        mpp_api_->encode_put_frame(mpp_ctx_, input_frame);  // 核心操作： 将输入帧送入编码器进行编码
    if (ret != MPP_OK)
    {
        mpp_frame_deinit(&input_frame);  // 释放输入帧资源，无需释放与其绑定的 Buffer
        return -1;
    }
    ret = mpp_frame_deinit(&input_frame);  // 释放输入帧资源，无需释放与其绑定的 Buffer
    if (ret != MPP_OK)
    {
        return -1;
    }
    input_frame = nullptr;  // 避免悬空指针
    return 0;
}

int MppEncInstance::EncoderGetExtraInfoPacket(EncPacketView* out)
{
    if (!out || !mpp_api_ || !mpp_ctx_)
    {
        return -1;
    }
    ResetPacketView(out);

    if (!HeaderBuf_)
    {
        return -1;
    }

    MppPacket packet = nullptr;
    MPP_RET   ret    = mpp_packet_init_with_buffer(&packet, HeaderBuf_);
    if (ret != MPP_OK || !packet)
    {
        return -1;
    }
    mpp_packet_set_length(packet, 0);

    ret = mpp_api_->control(mpp_ctx_, MPP_ENC_GET_HDR_SYNC, packet);
    if (ret != MPP_OK)
    {
        mpp_packet_deinit(&packet);
        return -1;
    }

    void*  ptr = mpp_packet_get_pos(packet);
    size_t len = mpp_packet_get_length(packet);
    if (!ptr || len == 0)
    {
        mpp_packet_deinit(&packet);
        return -1;
    }

    out->data        = ptr;
    out->len         = len;
    out->dts_ms      = -1;
    out->pts_ms      = -1;
    out->eos         = false;
    out->is_extra    = true;
    out->handle      = packet;
    ExtraInfoGotten_ = true;
    return 0;
}

int MppEncInstance::EncoderGetPacket(EncPacketView* out)
{
    if (!out || !mpp_api_ || !mpp_ctx_)
    {
        return mpp_enc_packet_result::kError;
    }
    ResetPacketView(out);

    MppPacket packet = nullptr;
    MPP_RET   ret    = mpp_api_->encode_get_packet(mpp_ctx_, &packet);
    if (ret == MPP_ERR_TIMEOUT)
    {
        return mpp_enc_packet_result::kNoPacket;
    }
    if (ret != MPP_OK)
    {
        return mpp_enc_packet_result::kError;
    }
    if (!packet)
    {
        return mpp_enc_packet_result::kNoPacket;
    }

    void*  ptr = mpp_packet_get_pos(packet);
    size_t len = mpp_packet_get_length(packet);
    if (!ptr || len == 0)
    {
        mpp_packet_deinit(&packet);
        return mpp_enc_packet_result::kNoPacket;
    }

    out->data     = ptr;
    out->len      = len;
    out->dts_ms   = mpp_packet_get_dts(packet);
    out->pts_ms   = mpp_packet_get_pts(packet);
    out->eos      = (mpp_packet_get_eos(packet) != 0);
    out->is_extra = false;
    out->handle   = packet;

    if (out->eos)
    {
        ReachPacketEOS_ = true;
    }
    return mpp_enc_packet_result::kHasPacket;
}

int MppEncInstance::EncoderReleasePacket(EncPacketView* pkt)
{
    if (!pkt)
    {
        return -1;
    }
    if (pkt->handle)
    {
        MPP_RET ret = mpp_packet_deinit(&pkt->handle);
        if (ret != MPP_OK)
        {
            return -1;
        }
    }
    ResetPacketView(pkt);
    return 0;
}
