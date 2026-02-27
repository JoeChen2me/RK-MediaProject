#include "v4l2Camera.h"

#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cmath>
#include <cerrno>
#include <cstring>
#include <iostream>

namespace
{
constexpr const char* kCameraDmaHeapPath = "/dev/dma_heap/system";

int alloc_dma_buf_fd(size_t size, int* out_fd, void** out_base)
{
    if (!out_fd || !out_base || size == 0)
    {
        return -1;
    }

    int heap_fd = open(kCameraDmaHeapPath, O_RDWR | O_CLOEXEC);
    if (heap_fd < 0)
    {
        std::cerr << "Failed to open dma heap " << kCameraDmaHeapPath << ": "
                  << std::strerror(errno) << std::endl;
        return -1;
    }

    dma_heap_allocation_data req{};
    req.len      = size;
    req.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &req) < 0)
    {
        std::cerr << "Failed to allocate dma-buf from heap: " << std::strerror(errno) << std::endl;
        close(heap_fd);
        return -1;
    }
    close(heap_fd);

    void* mapped = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, req.fd, 0);
    if (mapped == MAP_FAILED)
    {
        std::cerr << "Failed to mmap dma-buf fd=" << req.fd << ": " << std::strerror(errno)
                  << std::endl;
        close(req.fd);
        return -1;
    }

    *out_fd   = req.fd;
    *out_base = mapped;
    return 0;
}

bool dma_buf_sync_read(int fd, bool is_start)
{
    if (fd < 0)
    {
        return false;
    }

    dma_buf_sync sync{};
    sync.flags = (is_start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) | DMA_BUF_SYNC_READ;
    if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) < 0)
    {
        return false;
    }
    return true;
}
}  // namespace

V4L2_Camera::V4L2_Camera()
{
    // std::cout << "V4L2_Camera constructor called" << std::endl;
}

V4L2_Camera::~V4L2_Camera()
{
    if (isStreamOn)
    {
        capture_stream_switch(false);  // 停止流式捕获
    }
    if (NumBuffers > 0)
    {
        deinit_camera_buffer();  // 解除缓冲区映射
    }
    if (camera_fd >= 0)
    {
        close_camera();  // 关闭摄像头设备
    }
    // std::cout << "V4L2_Camera destructor called" << std::endl;
}

int V4L2_Camera::camera_GlobalInit(const std::string& device, const int exposureMs)
{
    if (camera_fd >= 0 || NumBuffers > 0 || isStreamOn)
    {
        std::cerr
            << "Camera is already initialized, please recreate object before re-initialization"
            << std::endl;
        return -1;
    }

    bool opened        = false;
    bool buffersInited = false;
    bool streamStarted = false;

    if (open_camera(device) != 0)
    {
        std::cerr << "Failed to open camera during global initialization" << std::endl;
        goto fail;
    }
    opened = true;

    if (check_cameraCapabilities() != 0)
    {
        std::cerr << "Camera does not meet the required capabilities during global initialization"
                  << std::endl;
        goto fail;
    }

    if (init_camera_buffer() != 0)
    {
        std::cerr << "Failed to initialize camera buffers during global initialization"
                  << std::endl;
        goto fail;
    }
    buffersInited = true;

    if (set_exposure_time(exposureMs) != 0)
    {
        std::cerr << "Failed to set exposure time during global initialization" << std::endl;
        goto fail;
    }

    if (capture_stream_switch(true) != 0)
    {
        std::cerr << "Failed to start streaming capture during global initialization" << std::endl;
        goto fail;
    }
    streamStarted = true;

    return 0;

fail:
    if (streamStarted)
    {
        capture_stream_switch(false);
    }
    if (buffersInited)
    {
        deinit_camera_buffer();
    }
    if (opened && camera_fd >= 0)
    {
        if (close(camera_fd) != 0)
        {
            std::cerr << "Rollback close failed: " << std::strerror(errno) << std::endl;
        }
        camera_fd          = -1;
        isStreamOn         = false;
        isStreamingSupport = false;
    }
    return -1;
}

int V4L2_Camera::open_camera(const char* device)
{
    if (device == nullptr)
    {
        std::cerr << "Camera device path is null" << std::endl;
        return -1;
    }

    if (camera_fd >= 0)
    {
        std::cout << "Camera already opened: fd=" << camera_fd << std::endl;
        return 0;
    }

    // std::cout << "Opening camera device: " << device << std::endl;
    camera_fd = open(device, O_RDWR);
    if (camera_fd < 0)
    {
        std::cerr << "Failed to open " << device << ": " << std::strerror(errno) << std::endl;
        return -1;
    }
    return 0;
}

int V4L2_Camera::open_camera(const std::string& device) { return open_camera(device.c_str()); }

/**
 * @brief Checks the capabilities of the opened camera device.
 * 检查摄像头是否支持视频输出、按照指定的输出格式、分辨率、帧率，设置曝光时间等功能
 */
int V4L2_Camera::check_cameraCapabilities()
{
    if (camera_fd < 0)
    {
        std::cerr << "Camera is not opened" << std::endl;
        return -1;
    }

    // Reset cached capability flags for every fresh check.
    isStreamingSupport        = false;
    isStreamOn                = false;
    isMJPEGSupport            = false;
    isTargetResolutionSupport = false;
    isTargetFpsSupport        = false;
    ActiveConfig              = {};

    if (ioctl(camera_fd, VIDIOC_QUERYCAP, &cap) < 0)
    {
        std::cerr << "Failed to query camera capabilities: " << std::strerror(errno) << std::endl;
        return -1;
    }
    std::cout << "Camera Capabilities:" << std::endl;
    std::cout << "  Driver: " << cap.driver << std::endl;
    std::cout << "  Card: " << cap.card << std::endl;
    // std::cout << "  Bus Info: " << cap.bus_info << std::endl;
    // std::cout << "  Version: " << ((cap.version >> 16) & 0xFF) << "." << ((cap.version >> 8) &
    // 0xFF)
    //           << "." << (cap.version & 0xFF) << std::endl;
    // 检查视频输出的支持
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
    {
        std::cerr << "Camera does not support video capture" << std::endl;
        return -1;
    }
    if (!(cap.capabilities & V4L2_CAP_STREAMING))
    {
        std::cerr << "Camera does not support streaming I/O" << std::endl;
        return -1;
    }
    this->isStreamingSupport = true;  // 标记支持流式传输
    // 检查输出格式的支持
    std::memset(&fmtdesc, 0, sizeof(fmtdesc));
    fmtdesc.index = 0;
    fmtdesc.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    while (ioctl(camera_fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0)
    {
        fmtdesc.index++;
        if (fmtdesc.pixelformat == V4L2_PIX_FMT_MJPEG)
        {
            std::cout << "    - Supported format: MJPEG" << std::endl;
            this->isMJPEGSupport = true;
        }
    }
    if (this->isMJPEGSupport == false)
    {
        std::cerr << "Camera does not support MJPEG format" << std::endl;
        return -1;
    }
    // 基于 MJPEG 格式设置分辨率
    std::memset(&frmsize, 0, sizeof(frmsize));
    frmsize.index = 0;
    // frmsize.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; 这里不需要设置 type，这里的 type 是返回值
    frmsize.pixel_format = V4L2_PIX_FMT_MJPEG;
    while (ioctl(camera_fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0)
    {
        frmsize.index++;
        if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE)
        {
            //   std::cout << "    - Supported resolution: " << frmsize.discrete.width
            //             << "x" << frmsize.discrete.height << std::endl;
            if (frmsize.discrete.width == camera_params::kCaptureWidth &&
                frmsize.discrete.height == camera_params::kCaptureHeight)
            {
                std::cout << "    - Supported resolution: " << frmsize.discrete.width << "x"
                          << frmsize.discrete.height << std::endl;
                this->isTargetResolutionSupport = true;
            }
        }
    }
    if (this->isTargetResolutionSupport == false)
    {
        std::cerr << "Camera does not support " << camera_params::kCaptureWidth << "x"
                  << camera_params::kCaptureHeight << " resolution" << std::endl;
        return -1;
    }

    // 基于 MJPEG 格式和目标分辨率设置帧率
    std::memset(&frmival, 0, sizeof(frmival));
    frmival.index        = 0;
    frmival.pixel_format = V4L2_PIX_FMT_MJPEG;
    frmival.width        = camera_params::kCaptureWidth;
    frmival.height       = camera_params::kCaptureHeight;
    while (ioctl(camera_fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmival) == 0)
    {
        frmival.index++;
        if (frmival.type == V4L2_FRMIVAL_TYPE_DISCRETE)
        {
            double fps = static_cast<double>(frmival.discrete.denominator) /
                         frmival.discrete.numerator;  // 分母除分子得到帧率
            //   std::cout << "    - Supported frame rate: " << fps << " fps" <<
            //   std::endl;
            if (std::abs(fps - static_cast<double>(camera_params::kCaptureFps)) <
                0.1)  // 考虑浮点数计算的误差
            {
                std::cout << "    - Supported frame rate: " << fps << " fps" << std::endl;
                this->isTargetFpsSupport = true;
            }
        }
    }
    if (this->isTargetFpsSupport == false)
    {
        std::cerr << "Camera does not support " << camera_params::kCaptureFps
                  << "fps frame rate at " << camera_params::kCaptureWidth << "x"
                  << camera_params::kCaptureHeight << " resolution" << std::endl;
        return -1;
    }

    // 开始设置
    std::memset(&v4l2fmt, 0, sizeof(v4l2fmt));
    v4l2fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    v4l2fmt.fmt.pix.width       = camera_params::kCaptureWidth;
    v4l2fmt.fmt.pix.height      = camera_params::kCaptureHeight;
    v4l2fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    v4l2fmt.fmt.pix.field       = V4L2_FIELD_NONE;  // 设置为逐行扫描
    if (ioctl(camera_fd, VIDIOC_S_FMT, &v4l2fmt) < 0)
    {
        std::cerr << "Failed to set pixel format: " << std::strerror(errno) << std::endl;
        return -1;
    }
    // 二次确认：S_FMT 后立即 G_FMT，读取驱动最终生效配置。
    {
        struct v4l2_format fmt_get
        {};
        fmt_get.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(camera_fd, VIDIOC_G_FMT, &fmt_get) < 0)
        {
            std::cerr << "Failed to get pixel format after VIDIOC_S_FMT: " << std::strerror(errno)
                      << std::endl;
            return -1;
        }
        auto fourcc_to_str = [](__u32 f)
        {
            char s[5] = {static_cast<char>(f & 0xFF), static_cast<char>((f >> 8) & 0xFF),
                         static_cast<char>((f >> 16) & 0xFF), static_cast<char>((f >> 24) & 0xFF),
                         '\0'};
            return std::string(s);
        };
        ActiveConfig.width        = fmt_get.fmt.pix.width;
        ActiveConfig.height       = fmt_get.fmt.pix.height;
        ActiveConfig.pixelformat  = fmt_get.fmt.pix.pixelformat;
        ActiveConfig.bytesperline = fmt_get.fmt.pix.bytesperline;
        ActiveConfig.sizeimage    = fmt_get.fmt.pix.sizeimage;
        std::cout << "[V4L2] Active format:"
                  << " width=" << ActiveConfig.width << " height=" << ActiveConfig.height
                  << " pixfmt=" << fourcc_to_str(ActiveConfig.pixelformat)
                  << " bytesperline=" << ActiveConfig.bytesperline
                  << " sizeimage=" << ActiveConfig.sizeimage << std::endl;
    }
    // 必须保持 MJPEG，宽高允许驱动协商后调整（后续按 ActiveConfig 传递）。
    if (ActiveConfig.pixelformat != V4L2_PIX_FMT_MJPEG)
    {
        std::cerr << "Camera did not keep MJPEG format, active pixelformat=0x" << std::hex
                  << ActiveConfig.pixelformat << std::dec << std::endl;
        return -1;
    }
    if (ActiveConfig.width != camera_params::kCaptureWidth ||
        ActiveConfig.height != camera_params::kCaptureHeight)
    {
        std::cerr << "Warning: active resolution is " << ActiveConfig.width << "x"
                  << ActiveConfig.height << ", requested " << camera_params::kCaptureWidth << "x"
                  << camera_params::kCaptureHeight << std::endl;
    }
    // 确认是否支持设置帧率
    std::memset(&streamparm, 0, sizeof(streamparm));
    streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(camera_fd, VIDIOC_G_PARM, &streamparm) < 0)
    {
        std::cerr << "Failed to get stream parameters: " << std::strerror(errno) << std::endl;
        return -1;
    }
    if (!(streamparm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME))
    {
        std::cerr << "Camera does not support setting frame rate" << std::endl;
        return -1;
    }
    // 支持帧率设置 开始设置帧率
    std::memset(&streamparm, 0, sizeof(streamparm));
    streamparm.type                                  = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    streamparm.parm.capture.timeperframe.numerator   = 1;                           // 分子
    streamparm.parm.capture.timeperframe.denominator = camera_params::kCaptureFps;  // 分母
    if (ioctl(camera_fd, VIDIOC_S_PARM, &streamparm) < 0)
    {
        std::cerr << "Failed to set frame rate: " << std::strerror(errno) << std::endl;
        return -1;
    }
    // 二次确认：S_PARM 后立即 G_PARM，读取驱动最终生效帧率。
    {
        struct v4l2_streamparm parm_get
        {};
        parm_get.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(camera_fd, VIDIOC_G_PARM, &parm_get) < 0)
        {
            std::cerr << "Failed to get stream parameters after VIDIOC_S_PARM: "
                      << std::strerror(errno) << std::endl;
            return -1;
        }
        double fps_verify = 0.0;
        if (parm_get.parm.capture.timeperframe.numerator != 0)
        {
            fps_verify = static_cast<double>(parm_get.parm.capture.timeperframe.denominator) /
                         parm_get.parm.capture.timeperframe.numerator;
        }
        ActiveConfig.fps_num = parm_get.parm.capture.timeperframe.numerator;
        ActiveConfig.fps_den = parm_get.parm.capture.timeperframe.denominator;
        std::cout << "[V4L2] Active frame rate:"
                  << " tpf=" << ActiveConfig.fps_num << "/" << ActiveConfig.fps_den
                  << " fps=" << fps_verify << std::endl;
    }
    // 基于 G_PARM 的读回结果进行告警判断，避免使用 S_PARM 请求值造成误判。
    if (ActiveConfig.fps_num == 0)
    {
        std::cerr << "Invalid frame rate returned by driver" << std::endl;
        return -1;
    }
    double fpsActual =
        static_cast<double>(ActiveConfig.fps_den) / static_cast<double>(ActiveConfig.fps_num);
    if (std::abs(fpsActual - static_cast<double>(camera_params::kCaptureFps)) > 0.1)
    {
        std::cerr << "Warning: active frame rate is " << fpsActual << " fps, requested "
                  << camera_params::kCaptureFps << " fps" << std::endl;
    }
    ActiveConfig.valid = true;
    std::cout << "Camera capabilities check passed.\nCamera Init Done " << std::endl;
    return 0;
}

/**
 * @brief Sets camera exposure mode/time.
 *
 * If exposure_time_ms is 0, this function enables auto exposure.
 * If exposure_time_ms is greater than 0, it switches to manual exposure,
 * checks the supported range of V4L2_CID_EXPOSURE_ABSOLUTE, and sets it.
 *
 * @param exposure_time_ms Exposure time in milliseconds.
 * @return 0 on success, -1 on failure.
 */
int V4L2_Camera::set_exposure_time(int exposure_time_ms)
{
    if (camera_fd < 0)
    {
        std::cerr << "Camera is not opened" << std::endl;
        return -1;
    }

    if (exposure_time_ms < 0)
    {
        std::cerr << "Invalid exposure time: " << exposure_time_ms << " ms (must be >= 0)"
                  << std::endl;
        return -1;
    }

    if (exposure_time_ms == 0)  // 自动曝光模式
    {
        v4l2_queryctrl query{};
        query.id = V4L2_CID_EXPOSURE_AUTO;
        if (ioctl(camera_fd, VIDIOC_QUERYCTRL, &query) < 0)
        {
            std::cerr << "Failed to query auto exposure control: " << std::strerror(errno)
                      << std::endl;
            return -1;
        }

        if (query.flags & V4L2_CTRL_FLAG_DISABLED)
        {
            std::cerr << "Auto exposure control is disabled by driver" << std::endl;
            return -1;
        }

        auto mode_supported = [&](int mode) -> bool
        {
            if (mode < query.minimum || mode > query.maximum)
            {
                return false;
            }
            if (query.type == V4L2_CTRL_TYPE_MENU || query.type == V4L2_CTRL_TYPE_INTEGER_MENU)
            {
                v4l2_querymenu menu{};
                menu.id    = V4L2_CID_EXPOSURE_AUTO;
                menu.index = static_cast<__u32>(mode);
                if (ioctl(camera_fd, VIDIOC_QUERYMENU, &menu) < 0)
                {
                    return false;
                }
            }
            return true;
        };

        auto mode_name = [](int mode) -> const char*
        {
            switch (mode)
            {
                case V4L2_EXPOSURE_AUTO:
                    return "Auto Mode";
                case V4L2_EXPOSURE_MANUAL:
                    return "Manual Mode";
                case V4L2_EXPOSURE_SHUTTER_PRIORITY:
                    return "Shutter Priority Mode";
                case V4L2_EXPOSURE_APERTURE_PRIORITY:
                    return "Aperture Priority Mode";
                default:
                    return "Unknown Mode";
            }
        };

        // 有些 UVC 摄像头不支持 V4L2_EXPOSURE_AUTO(0)，只支持 Aperture Priority(3)。
        // 这里按“真正自动优先，其次光圈优先”尝试，避免 EINVAL。
        const int auto_mode_candidates[] = {
            V4L2_EXPOSURE_AUTO,
            V4L2_EXPOSURE_APERTURE_PRIORITY,
            V4L2_EXPOSURE_SHUTTER_PRIORITY,
        };

        int last_errno = 0;
        for (int mode : auto_mode_candidates)
        {
            if (!mode_supported(mode))
            {
                continue;
            }

            v4l2_control auto_ctrl{};
            auto_ctrl.id    = V4L2_CID_EXPOSURE_AUTO;
            auto_ctrl.value = mode;
            if (ioctl(camera_fd, VIDIOC_S_CTRL, &auto_ctrl) == 0)
            {
                std::cout << "Exposure mode set to " << mode_name(mode) << std::endl;
                return 0;
            }
            last_errno = errno;
        }

        if (last_errno != 0)
        {
            std::cerr << "Failed to set auto exposure mode: " << std::strerror(last_errno)
                      << std::endl;
        }
        else
        {
            std::cerr << "No supported auto exposure mode is available on this camera" << std::endl;
        }
        return -1;
    }

    // Most UVC drivers use V4L2_CID_EXPOSURE_ABSOLUTE in 100us units.
    const int exposure_100us = exposure_time_ms * 10;

    v4l2_control auto_ctrl{};
    auto_ctrl.id    = V4L2_CID_EXPOSURE_AUTO;
    auto_ctrl.value = V4L2_EXPOSURE_MANUAL;
    if (ioctl(camera_fd, VIDIOC_S_CTRL, &auto_ctrl) < 0)
    {
        std::cerr << "Failed to set manual exposure mode: " << std::strerror(errno) << std::endl;
        return -1;
    }

    v4l2_queryctrl query{};
    query.id = V4L2_CID_EXPOSURE_ABSOLUTE;
    if (ioctl(camera_fd, VIDIOC_QUERYCTRL, &query) < 0)
    {
        std::cerr << "Failed to query exposure range: " << std::strerror(errno) << std::endl;
        return -1;
    }

    if (query.flags & V4L2_CTRL_FLAG_DISABLED)
    {
        std::cerr << "Exposure control is disabled by driver" << std::endl;
        return -1;
    }

    if (exposure_100us < query.minimum || exposure_100us > query.maximum)
    {
        std::cerr << "Exposure out of range: " << exposure_time_ms << " ms"
                  << " (supported " << (query.minimum / 10.0) << "-" << (query.maximum / 10.0)
                  << " ms)" << std::endl;
        return -1;
    }

    v4l2_control exp_ctrl{};
    exp_ctrl.id    = V4L2_CID_EXPOSURE_ABSOLUTE;
    exp_ctrl.value = exposure_100us;
    if (ioctl(camera_fd, VIDIOC_S_CTRL, &exp_ctrl) < 0)
    {
        std::cerr << "Failed to set exposure time: " << std::strerror(errno) << std::endl;
        return -1;
    }

    std::cout << "Exposure time set to " << exposure_time_ms << " ms" << std::endl;
    return 0;
}

int V4L2_Camera::init_camera_buffer()
{
    // 基础检查
    if (camera_fd == -1)
    {
        std::cerr << "Camera is not opened" << std::endl;
        return -1;
    }
    if (this->isStreamingSupport == false)
    {
        std::cerr << "Camera does not support streaming I/O" << std::endl;
        return -1;
    }
    if (NumBuffers > 0)
    {
        std::cerr << "Camera buffers already initialized" << std::endl;
        return -1;
    }

    NumBuffers       = 0;
    CurrentFrameDesc = nullptr;
    for (auto& frame_desc : FrameDescArray)
    {
        frame_desc.index             = -1;
        frame_desc.fd                = -1;
        frame_desc.base              = nullptr;
        frame_desc.Length            = 0;
        frame_desc.payloadSize       = 0;
        frame_desc.cpuSyncReadActive = false;
    }

    // 请求缓冲区
    struct v4l2_requestbuffers reqbuf
    {};
    const __u32 requested_count = camera_params::kRequestBufferCount;
    reqbuf.count                = requested_count;              // 请求的缓冲区数量
    reqbuf.type                 = V4L2_BUF_TYPE_VIDEO_CAPTURE;  // 视频捕获
    reqbuf.memory               = V4L2_MEMORY_DMABUF;           // 使用 DMABUF 方式
    if (ioctl(camera_fd, VIDIOC_REQBUFS, &reqbuf) < 0)
    {
        std::cerr << "Failed to request DMABUF capture buffers: " << std::strerror(errno)
                  << std::endl;
        return -1;
    }
    if (reqbuf.count == 0)
    {
        std::cerr << "Driver returned zero capture buffers" << std::endl;
        return -1;
    }
    if (reqbuf.count > camera_params::kMaxMappedBuffers)
    {
        std::cerr << "Driver returned too many buffers: " << reqbuf.count
                  << ", max supported: " << camera_params::kMaxMappedBuffers << std::endl;
        return -1;
    }
    std::cout << "[V4L2] IO mode: dmabuf-pool" << std::endl;
    std::cout << "[V4L2] Buffer request: requested=" << requested_count
              << ", granted=" << reqbuf.count << std::endl;

    // DMABUF 自建池：用户分配缓冲，驱动按 index 使用这些 fd
    const size_t alloc_size = (ActiveConfig.sizeimage > 0)
                                  ? static_cast<size_t>(ActiveConfig.sizeimage)
                                  : camera_params::kMaxFrameSize;
    for (__u32 i = 0; i < reqbuf.count; ++i)
    {
        int   dma_fd   = -1;
        void* dma_base = nullptr;
        if (alloc_dma_buf_fd(alloc_size, &dma_fd, &dma_base) !=
            0)  // 这里才是实际发生内存申请的地方
        {
            std::cerr << "Failed to allocate DMABUF for camera pool, index=" << i << std::endl;
            deinit_camera_buffer();
            return -1;
        }

        FrameDescArray[i].index  = static_cast<int>(i);  // 后续利用 index 来查找
        FrameDescArray[i].fd     = dma_fd;
        FrameDescArray[i].base   = dma_base;
        FrameDescArray[i].Length = alloc_size;
        this->NumBuffers++;
    }

    // 将所有缓冲区入队，准备开始捕获
    for (int i = 0; i < this->NumBuffers; i++)
    {
        struct v4l2_buffer buffer
        {};
        buffer.index = static_cast<__u32>(
            i);  // 这里的 index 保持和 FrameDescArray[i].index 一致，方便后续通过 index 查找对应的
                 // DMABUF fd 和用户态映射地址
        buffer.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_DMABUF;
        buffer.m.fd = FrameDescArray[i].fd;  // 在这里告知驱动使用用户提供的 DMABUF fd
        buffer.length = static_cast<__u32>(FrameDescArray[i].Length);

        if (ioctl(camera_fd, VIDIOC_QBUF, &buffer) < 0)
        {
            std::cerr << "Failed to queue buffer " << buffer.index << ": " << std::strerror(errno)
                      << std::endl;
            deinit_camera_buffer();
            return -1;
        }
    }
    return 0;
}

int V4L2_Camera::deinit_camera_buffer()
{
    if (this->NumBuffers <= 0)
    {
        return 0;
    }

    int ret = 0;
    for (int i = 0; i < this->NumBuffers; i++)
    {
        if (FrameDescArray[i].cpuSyncReadActive && FrameDescArray[i].fd >= 0)
        {
            if (!dma_buf_sync_read(FrameDescArray[i].fd, false))
            {
                std::cerr << "Failed to end dma-buf cpu read sync for buffer " << i << ": "
                          << std::strerror(errno) << std::endl;
                ret = -1;
            }
            FrameDescArray[i].cpuSyncReadActive = false;
        }

        // 先解除用户态映射
        if (FrameDescArray[i].base != nullptr && FrameDescArray[i].Length > 0)
        {
            if (munmap(FrameDescArray[i].base, FrameDescArray[i].Length) != 0)
            {
                std::cerr << "Failed to munmap buffer " << i << ": " << std::strerror(errno)
                          << std::endl;
                ret = -1;
            }
            FrameDescArray[i].base        = nullptr;
            FrameDescArray[i].Length      = 0;
            FrameDescArray[i].payloadSize = 0;
        }

        // 再关闭导出的 dma-buf 文件描述符
        if (FrameDescArray[i].fd >= 0)
        {
            if (close(FrameDescArray[i].fd) != 0)
            {
                std::cerr << "Failed to close exported dma-buf fd for buffer " << i << ": "
                          << std::strerror(errno) << std::endl;
                ret = -1;
            }
            FrameDescArray[i].fd = -1;
        }
        FrameDescArray[i].index = -1;
    }

    struct v4l2_requestbuffers reqbuf
    {};
    reqbuf.count  = 0;  // 释放驱动侧分配的全部缓冲区
    reqbuf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    reqbuf.memory = V4L2_MEMORY_DMABUF;
    if (ioctl(camera_fd, VIDIOC_REQBUFS, &reqbuf) < 0)
    {
        std::cerr << "Failed to release V4L2 buffers: " << std::strerror(errno) << std::endl;
        ret = -1;
    }

    NumBuffers       = 0;
    CurrentFrameDesc = nullptr;
    return ret;
}

int V4L2_Camera::capture_stream_switch(bool enabled)
{
    if (camera_fd < 0)
    {
        std::cerr << "Camera is not opened" << std::endl;
        return -1;
    }
    if (this->isStreamingSupport == false)
    {
        std::cerr << "Camera does not support streaming I/O" << std::endl;
        return -1;
    }
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;  // 视频捕获类型
    if (enabled)
    {
        if (isStreamOn)
        {
            return 0;
        }
        if (ioctl(camera_fd, VIDIOC_STREAMON, &type) < 0)  // 开始流式捕获
        {
            std::cerr << "Failed to start streaming: " << std::strerror(errno) << std::endl;
            return -1;
        }
        isStreamOn = true;
        std::cout << "Streaming started" << std::endl;
    }
    else
    {
        if (!isStreamOn)
        {
            return 0;
        }
        if (ioctl(camera_fd, VIDIOC_STREAMOFF, &type) < 0)  // 停止流式捕获
        {
            std::cerr << "Failed to stop streaming: " << std::strerror(errno) << std::endl;
            return -1;
        }
        isStreamOn = false;
        std::cout << "Streaming stopped" << std::endl;
    }
    return 0;
}

int V4L2_Camera::camera_read_frame(size_t* out_length, size_t max_buffer_size)
{
    if (camera_fd < 0)
    {
        std::cerr << "Camera is not opened" << std::endl;
        return camera_read_result::kFatal;
    }
    if (this->isStreamingSupport == false)
    {
        std::cerr << "Camera does not support streaming I/O" << std::endl;
        return camera_read_result::kFatal;
    }
    if (!isStreamOn)
    {
        std::cerr << "Camera stream is not started" << std::endl;
        return camera_read_result::kFatal;
    }
    if (out_length == nullptr)
    {
        std::cerr << "Invalid output buffer arguments" << std::endl;
        return camera_read_result::kFatal;
    }

    CurrentFrameDesc = nullptr;

    unsigned int bufIdx    = 0;
    unsigned int frameSize = 0;
    const int    read_rc =
        dequeue_buffer(bufIdx, frameSize,
                       max_buffer_size);  // 从驱动队列中取出一帧数据，得到对应的缓冲区索引和帧大小
    if (read_rc != camera_read_result::kOk)
    {
        *out_length = frameSize;
        return read_rc;
    }
    if (FrameDescArray[bufIdx].base == nullptr)
    {
        std::cerr << "Mapped buffer base is null for index: " << bufIdx << std::endl;
        if (requeue_buffer(&FrameDescArray[bufIdx]) != 0)
        {
            std::cerr << "Failed to requeue buffer after null mapped buffer" << std::endl;
        }
        return camera_read_result::kFatal;
    }

    FrameDescArray[bufIdx].payloadSize = frameSize;
    *out_length                        = frameSize;
    CurrentFrameDesc = &FrameDescArray[bufIdx];  // 记录当前帧描述，供外部解码直接使用
    return camera_read_result::kOk;
}

int V4L2_Camera::dequeue_buffer(unsigned int& BufIdx, unsigned int& bufferSize,
                                const size_t maxBufferSize)
{
    bufferSize = 0;

    // 使用 poll 来实现非阻塞等待，避免直接调用 DQBUF 可能导致的长时间阻塞
    struct pollfd pfd
    {};
    pfd.fd             = camera_fd;
    pfd.events         = POLLIN | POLLPRI;  // 等待可读事件
    const int poll_ret = poll(&pfd, 1, camera_params::kDequeueTimeoutMs);
    if (poll_ret == 0)  // 超时没有事件发生，安全地返回超时结果，避免长时间阻塞在 DQBUF 上
    {
        std::cerr << "Dequeue timeout after " << camera_params::kDequeueTimeoutMs << " ms"
                  << std::endl;
        return camera_read_result::kTimeout;
    }
    if (poll_ret < 0)  // poll 出错，可能是中断或其他错误，根据 errno 判断是否重试
    {
        if (errno == EINTR || errno == EAGAIN)
        {
            return camera_read_result::kRetryable;
        }
        std::cerr << "Failed to poll camera fd before dequeue: " << std::strerror(errno)
                  << std::endl;
        return camera_read_result::kFatal;
    }
    if (pfd.revents & (POLLERR | POLLHUP |
                       POLLNVAL))  // 监测异常事件，避免在异常状态下调用 DQBUF 导致更严重的问题
    {
        std::cerr << "Poll reported camera fd abnormal revents=0x" << std::hex << pfd.revents
                  << std::dec << std::endl;
        return camera_read_result::kFatal;
    }
    if ((pfd.revents & (POLLIN | POLLPRI)) ==
        0)  // 没有可读事件，可能是误报或其他事件，安全起见不调用 DQBUF
    {
        return camera_read_result::kRetryable;
    }
    /**
     * 在确认有可读事件后，安全地调用 DQBUF 来获取帧数据。即使在极少数情况下 poll 可能误报，但 DQBUF
     * 的错误处理也足够健壮，可以通过 errno 判断是否需要重试或处理错误。
     */
    struct v4l2_buffer buffer
    {};
    buffer.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;      // 视频捕获类型
    buffer.memory = V4L2_MEMORY_DMABUF;               // 固定使用 DMABUF
    if (ioctl(camera_fd, VIDIOC_DQBUF, &buffer) < 0)  // 从内核队列中取出一帧数据
    {
        if (errno == EINTR || errno == EAGAIN)
        {
            return camera_read_result::kRetryable;
        }
        std::cerr << "Failed to dequeue buffer: " << std::strerror(errno) << std::endl;
        return camera_read_result::kFatal;
    }
    if (static_cast<int>(buffer.index) >= this->NumBuffers)
    {
        std::cerr << "Invalid buffer index dequeued: " << buffer.index << std::endl;
        return camera_read_result::kFatal;
    }

    FrameDesc& frame_desc = FrameDescArray[buffer.index];
    if (!dma_buf_sync_read(frame_desc.fd, true))
    {
        std::cerr << "Failed to start dma-buf cpu read sync for buffer " << buffer.index << ": "
                  << std::strerror(errno) << std::endl;
        return camera_read_result::kFatal;
    }
    frame_desc.cpuSyncReadActive = true;

    bufferSize = buffer.bytesused;
    if (buffer.bytesused > maxBufferSize)
    {
        std::cerr << "Buffer size exceeds max allowed size: " << buffer.bytesused << " > "
                  << maxBufferSize << std::endl;

        if (frame_desc.cpuSyncReadActive)
        {
            if (!dma_buf_sync_read(frame_desc.fd, false))
            {
                std::cerr << "Failed to end dma-buf cpu read sync for oversized buffer "
                          << buffer.index << ": " << std::strerror(errno) << std::endl;
            }
            frame_desc.cpuSyncReadActive = false;
        }

        if (ioctl(camera_fd, VIDIOC_QBUF, &buffer) < 0)  // 将帧重新入队
        {
            std::cerr << "Failed to requeue buffer: " << std::strerror(errno) << std::endl;
        }
        return camera_read_result::kFatal;
    }
    BufIdx     = buffer.index;
    bufferSize = buffer.bytesused;
    return camera_read_result::kOk;
}

int V4L2_Camera::requeue_buffer(FrameDesc* frame_desc)
{
    if (frame_desc == nullptr)
    {
        std::cerr << "Invalid frame descriptor for requeue: null pointer" << std::endl;
        return -1;
    }

    const int idx = frame_desc->index;
    if (idx < 0 || idx >= NumBuffers)
    {
        std::cerr << "Invalid buffer index for requeue: " << idx << std::endl;
        return -1;
    }
    const auto buf_idx = static_cast<unsigned int>(idx);

    if (frame_desc->cpuSyncReadActive)
    {
        if (!dma_buf_sync_read(frame_desc->fd, false))
        {
            std::cerr << "Failed to end dma-buf cpu read sync for buffer " << buf_idx << ": "
                      << std::strerror(errno) << std::endl;
            return -1;
        }
        frame_desc->cpuSyncReadActive = false;
    }

    struct v4l2_buffer buffer
    {};
    buffer.index  = buf_idx;
    buffer.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;  // 视频捕获类型
    buffer.memory = V4L2_MEMORY_DMABUF;           // 固定使用 DMABUF
    buffer.m.fd   = FrameDescArray[buf_idx].fd;
    buffer.length = static_cast<__u32>(FrameDescArray[buf_idx].Length);
    if (ioctl(camera_fd, VIDIOC_QBUF, &buffer) < 0)  // 将帧重新入队
    {
        std::cerr << "Failed to requeue buffer: " << std::strerror(errno) << std::endl;
        return -1;
    }

    frame_desc->payloadSize             = 0;
    FrameDescArray[buf_idx].payloadSize = 0;
    if (CurrentFrameDesc == frame_desc || CurrentFrameDesc == &FrameDescArray[buf_idx])
    {
        CurrentFrameDesc = nullptr;
    }

    return 0;
}

int V4L2_Camera::close_camera()
{
    if (camera_fd < 0)
    {
        std::cout << "Camera already closed" << std::endl;
        return 0;
    }

    std::cout << "Closing camera device: fd=" << camera_fd << std::endl;
    if (close(camera_fd) != 0)
    {
        std::cerr << "Failed to close camera fd=" << camera_fd << ": " << std::strerror(errno)
                  << std::endl;
        return -1;
    }

    camera_fd          = -1;
    isStreamOn         = false;
    isStreamingSupport = false;
    return 0;
}
