#ifndef V4L2_CAMERA_H
#define V4L2_CAMERA_H

#include <linux/videodev2.h>

#include "pubDataType.h"

#include <cstddef>
#include <string>

namespace camera_params
{
inline constexpr __u32  kCaptureWidth       = 640;
inline constexpr __u32  kCaptureHeight      = 480;
inline constexpr __u32  kCaptureFps         = 30;
inline constexpr size_t kMaxFrameSize       = 1024 * 1024;
inline constexpr __u32  kRequestBufferCount = 4;
inline constexpr size_t kMaxMappedBuffers   = 10;
inline constexpr int    kDequeueTimeoutMs   = 10 * 1000;
}  // namespace camera_params

namespace camera_read_result
{
inline constexpr int kOk        = 0;
inline constexpr int kRetryable = 1;
inline constexpr int kTimeout   = 2;
inline constexpr int kFatal     = -1;
}  // namespace camera_read_result

class V4L2_Camera
{
   public:
    V4L2_Camera();
    ~V4L2_Camera();
    // int Camera_Init(const char* device);
    int camera_GlobalInit(
        const std::string& device,
        const int exposureMs = 0);  // 包含打开设备、检查能力、初始化缓冲区等步骤的全局初始化函数
    int open_camera(const char* device);         // 打开摄像头
    int open_camera(const std::string& device);  // 重载函数，接受 std::string 参数
    int check_cameraCapabilities();  // 检查摄像头的功能支持并设置输出格式、分辨率、帧率等参数
    int init_camera_buffer();        // 初始化摄像头缓冲区
    int deinit_camera_buffer();      // 反初始化摄像头缓冲区 用于解除映射
    int capture_stream_switch(bool enabled);  // 流式捕获开关
    int camera_read_frame(size_t* out_length,
                          size_t  max_buffer_size = camera_params::kMaxFrameSize);  // 读取一帧数据
    int dequeue_buffer(unsigned int& BufIdx, unsigned int& bufferSize,
                       const size_t maxBufferSize);  // 从内核中取出一个缓冲区进行读取
    int requeue_buffer(FrameDesc* frame_desc);       // 将一个缓冲区重新放回内核中以供后续使用

    int       set_exposure_time(int exposure_time_ms);
    FrameDesc FrameDescArray[camera_params::kMaxMappedBuffers];  // 保存每个缓冲区的统一描述信息
    int       get_buffer_count() const { return NumBuffers; }
    FrameDesc*   CurrentFrameDesc   = nullptr;  // 当前出队并等待处理的帧描述

   private:
    int close_camera();  // 仅供析构函数调用，外部不可见

    int                     camera_fd = -1;
    struct v4l2_capability  cap;
    struct v4l2_fmtdesc     fmtdesc;
    struct v4l2_frmsizeenum frmsize;
    struct v4l2_frmivalenum frmival;
    struct v4l2_format      v4l2fmt;
    struct v4l2_streamparm  streamparm;
    bool                    isStreamingSupport        = false;
    bool                    isStreamOn                = false;
    bool                    isMJPEGSupport            = false;
    bool                    isTargetResolutionSupport = false;
    bool                    isTargetFpsSupport        = false;
    int                     NumBuffers                = 0;  // 实际请求到的缓冲区数量
};

struct v4l2CapList
{
    bool isMJPEGSupport = false;
    bool isYUYVSupport  = false;
};

#endif  // V4L2_CAMERA_H
