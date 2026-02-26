#include "rkmppenc.h"

namespace
{
constexpr RK_S64      kMppTimeoutMs   = 500;
constexpr MppPollType kMppPollTimeout = static_cast<MppPollType>(500);
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
    if (this->enc_cfg_)
    {
        mpp_enc_cfg_deinit(this->enc_cfg_);
        this->enc_cfg_ = nullptr;
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
    ret           = mpp_init(mpp_ctx_, MPP_CTX_ENC, EncodingType_);  // 编码器类型 指定编码格式
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
    if (width == 0 || height == 0 || hor_stride == 0 || ver_stride == 0 || fps == 0 ||
        bitrate_bps == 0 || gop == 0)
    {
        return -1;
    }
    MPP_RET ret = MPP_NOK;
    ImgHeight_  = height;
    ImgWidth_   = width;
    H_Stride_   = hor_stride;
    V_Stride_   = ver_stride;
    Fps_        = (fps > 0 ? fps : 30);  // 默认 30 fps
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
    auto cleanup_import_resources = [&]()
    {
        for (size_t i = 0; i < count && i < resource_limits::kMppImportBufferCount; ++i)
        {
            if (MapBufferFromFD[i])
            {
                mpp_buffer_put(MapBufferFromFD[i]);
                MapBufferFromFD[i] = nullptr;
            }
        }
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
        info.type  = MPP_BUFFER_TYPE_EXT_DMA;
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
    mpp_frame_set_pts(*out_frame, static_cast<RK_S64>(InputFrameId++));  // 使用递增的帧 ID 作为 PTS
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
    // 申请可缓存的 DRM 内存池
    ret = mpp_buffer_group_get_internal(
        &group_, static_cast<MppBufferType>(MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_CACHABLE));
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
    MPP_RET  ret         = MPP_NOK;
    MppFrame input_frame = nullptr;
    int      BuildFrameRet =
        BuildInputFrameFromFd(input_desc, &input_frame);  // 构建输入帧并与 Buffer 进行绑定
    if (BuildFrameRet != 0 || !input_frame)
    {
        return -1;
    }
    ret = mpp_api_->encode_put_frame(mpp_ctx_, input_frame);
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

int MppEncInstance::EncoderGetPacket(void)
{
    if (!mpp_api_ || !mpp_ctx_)
    {
        return -1;
    }
    MPP_RET ret = MPP_NOK;
    while (true)
    {
        MppPacket packet = nullptr;
        ret              = mpp_api_->encode_get_packet(mpp_ctx_, &packet);
        if (ret != MPP_OK)
        {
            return -1;
        }
        if (packet == nullptr)
        {
            // 当前时刻无包可取，交给外层线程循环调度。
            return 0;
        }

        void*  ptr         = mpp_packet_get_pos(packet);
        size_t len         = mpp_packet_get_length(packet);
        RK_U32 packet_eos  = mpp_packet_get_eos(packet);
        RK_U32 packet_part = mpp_packet_is_partition(packet);
        RK_U32 packet_eoi  = mpp_packet_is_eoi(packet);
        RK_S64 packet_pts  = mpp_packet_get_pts(packet);
        RK_S64 packet_dts  = mpp_packet_get_dts(packet);
        // TODO 传递数据给 ZLMedia 来实现推流。由用户手动实现。但当前代码需要考虑这个点。
        (void)ptr;
        (void)len;
        (void)packet_pts;
        (void)packet_dts;

        // 资源回收
        mpp_packet_deinit(&packet);  // 释放 packet 资源

        // 分片中间包：继续拉取直到 eoi/eos。
        if (packet_part && !packet_eoi && !packet_eos)
        {
            continue;
        }
        // 非分片包、分片结尾包或 EOS 包，视为当前一帧输出结束，返回给外层调度。
        else
        {
            if (packet_eos)
            {
                ReachPacketEOS_ = true;  // 标记已到达输出 EOS
            }
            break;
        }
    }
    return 0;
}
