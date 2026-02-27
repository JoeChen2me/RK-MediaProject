#ifndef RKMPPENC_H
#define RKMPPENC_H

#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_mpi_cmd.h>

#include "pubDataType.h"       //导入公共数据类型头文件
#include "mpp_common_utils.h"  // 导入 MPP 相关的公共工具函数头文件
#include <array>
#include <cstddef>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <vector>

namespace
{
constexpr uint32_t kDefaultFps        = 30;               // 默认帧率
constexpr uint32_t kDefaultBitrateBps = 2 * 1024 * 1024;  // 默认码率 2 Mbps
constexpr uint32_t kDefaultGop        = 2 * kDefaultFps;  // 默认 GOP 长度为 2 秒
}  // namespace

namespace mpp_enc_packet_result
{
inline constexpr int kError     = -1;  // 调用失败
inline constexpr int kNoPacket  = 0;   // 当前时刻无可用输出包
inline constexpr int kHasPacket = 1;   // 成功获取到至少一个输出包
}  // namespace mpp_enc_packet_result

struct EncPacketView
{
    const void* data         = nullptr;  // 当前 packet 数据指针
    size_t      len          = 0;        // 当前 packet 长度
    int64_t     dts_ms       = -1;       // packet dts（毫秒）
    int64_t     pts_ms       = -1;       // packet pts（毫秒）
    bool        eos          = false;    // packet 是否带 EOS
    bool        is_partition = false;    // packet 是否为分片输出
    bool        is_eoi       = false;    // 分片输出中是否到达一帧结尾
    bool        is_extra     = false;    // 是否为编码头信息（SPS/PPS）
    MppPacket   handle       = nullptr;  // 需由 EncoderReleasePacket 释放
};

class MppEncInstance
{
   public:
    MppEncInstance();
    ~MppEncInstance();
    int  EncInit(MppCodingType EncodingType = MPP_VIDEO_CodingAVC);
    int  EncConfigWidthHeight(uint32_t width, uint32_t height, uint32_t hor_stride,
                              uint32_t ver_stride, uint32_t fps = kDefaultFps,
                              uint32_t bitrate_bps = kDefaultBitrateBps, uint32_t gop = kDefaultGop);
    int  EncodePushFrame(const IO_FD_t* input_desc);
    int  EncoderGetPacket(EncPacketView* out);
    int  EncoderGetExtraInfoPacket(EncPacketView* out);
    int  EncoderReleasePacket(EncPacketView* pkt);
    int  EncoderImportBufferFromFD(const IO_FD_t* FD_Array, size_t count);
    int  BuildInputFrameFromFd(const IO_FD_t* input_desc, MppFrame* out_frame, bool isEos = false);
    int  AllocBufferForIO(const IO_FD_t* FD_Array, size_t count);
    bool IsPacketEOS(void) const { return ReachPacketEOS_; }

   private:
    MppBuffer MapBufferFromFD[resource_limits::kMppImportBufferCount] = {
        nullptr};                             // 存储通过 fd 导入的 MppBuffer 句柄
    std::map<int, MppBuffer> ImportedFdMap_;  // 已导入的 dma-buf fd 与对应 MppBuffer 的映射
    MppCtx                   mpp_ctx_      = nullptr;              // 复用的 MPP 编码上下文
    MppApi*                  mpp_api_      = nullptr;              // 复用
    MppEncCfg                enc_cfg_      = nullptr;              // 编码配置句柄
    MppBufferGroup           group_        = nullptr;              //  内存池
    MppCodingType            EncodingType_ = MPP_VIDEO_CodingAVC;  // 编码类型
    uint32_t                 frame_count_  = 0;                    // 已编码帧计数
    uint32_t                 ImgWidth_     = 0;                    // 输入图像宽
    uint32_t                 ImgHeight_    = 0;                    // 输入图像高
    uint32_t                 H_Stride_     = 0;                    // 行对齐
    uint32_t                 V_Stride_     = 0;                    // 列对齐
    uint32_t                 Fps_          = 0;                    // 帧率
    uint32_t                 BitrateBps_   = 0;                    // 码率
    uint32_t                 Gop_          = 0;                    // 关键帧间隔
    size_t PacketBufSize_ = 0;  // 输出 packet 缓冲建议大小（按单帧上界估算）
    MppBufferGroup HeaderBufGroup_ = nullptr;  // 头信息抓取缓存组
    MppBuffer      HeaderBuf_      = nullptr;  // 头信息抓取缓存
    size_t         HeaderBufSize_  = 0;        // 头信息抓取缓存大小
    bool FD_Imported_     = false;  // 当前输入帧的 dma-buf fd 是否已成功导入到 MPP
    bool ExtraInfoGotten_ = false;  // 是否已获取过编码器输出的额外信息（如 SPS/PPS）
    uint64_t InputFrameId     = 0;      // 统计信息：输入帧 ID，递增以区分不同帧
    bool     AllocBufferDone_ = false;  // 是否已完成输入缓冲的分配和导入
    bool     ReachPacketEOS_  = false;  // 是否已到达输出 packet 的 EOS 标志
};

#endif  // RKMPPENC_H
