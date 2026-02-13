#include "v4l2Camera.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cmath>
#include <cerrno>
#include <cstring>
#include <iostream>

V4L2_Camera::V4L2_Camera()
{
  std::cout << "V4L2_Camera constructor called" << std::endl;
}

V4L2_Camera::~V4L2_Camera()
{
  if (camera_fd >= 0)
  {
    close_camera();
  }
  std::cout << "V4L2_Camera destructor called" << std::endl;
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

  std::cout << "Opening camera device: " << device << std::endl;
  camera_fd = open(device, O_RDWR);
  if (camera_fd < 0)
  {
    std::cerr << "Failed to open " << device << ": " << std::strerror(errno)
              << std::endl;
    return -1;
  }
  return 0;
}

int V4L2_Camera::open_camera(const std::string& device)
{
  return open_camera(device.c_str());
}

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
  isMJPEGSupport = false;
  is640x480Support = false;
  is30fpsSupport = false;

  if (ioctl(camera_fd, VIDIOC_QUERYCAP, &cap) < 0)
  {
    std::cerr << "Failed to query camera capabilities: " << std::strerror(errno)
              << std::endl;
    return -1;
  }
  std::cout << "Camera Capabilities:" << std::endl;
  std::cout << "  Driver: " << cap.driver << std::endl;
  std::cout << "  Card: " << cap.card << std::endl;
  std::cout << "  Bus Info: " << cap.bus_info << std::endl;
  std::cout << "  Version: " << ((cap.version >> 16) & 0xFF) << "."
            << ((cap.version >> 8) & 0xFF) << "." << (cap.version & 0xFF)
            << std::endl;
  // 检查视频输出的支持
  if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
  {
    std::cerr << "Camera does not support video capture" << std::endl;
    return -1;
  }
  // 检查输出格式的支持
  std::memset(&fmtdesc, 0, sizeof(fmtdesc));
  fmtdesc.index = 0;
  fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
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
  // 基于 MJPEG 格式设置分辨率为 640x480
  std::memset(&frmsize, 0, sizeof(frmsize));
  frmsize.index = 0;
  // frmsize.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; 这里不需要设置 type，因为我们在
  // ioctl 调用中已经指定了类型
  frmsize.pixel_format = V4L2_PIX_FMT_MJPEG;
  while (ioctl(camera_fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0)
  {
    frmsize.index++;
    if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE)
    {
      //   std::cout << "    - Supported resolution: " << frmsize.discrete.width
      //             << "x" << frmsize.discrete.height << std::endl;
      if (frmsize.discrete.width == 640 && frmsize.discrete.height == 480)
      {
        std::cout << "    - Supported resolution: " << frmsize.discrete.width
                  << "x" << frmsize.discrete.height << std::endl;
        this->is640x480Support = true;
      }
    }
  }
  if (this->is640x480Support == false)
  {
    std::cerr << "Camera does not support 640x480 resolution" << std::endl;
    return -1;
  }

  // 基于 MJPEG 格式和 640x480 分辨率设置帧率为 30fps
  std::memset(&frmival, 0, sizeof(frmival));
  frmival.index = 0;
  frmival.pixel_format = V4L2_PIX_FMT_MJPEG;
  frmival.width = 640;
  frmival.height = 480;
  while (ioctl(camera_fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmival) == 0)
  {
    frmival.index++;
    if (frmival.type == V4L2_FRMIVAL_TYPE_DISCRETE)
    {
      double fps = static_cast<double>(frmival.discrete.denominator) /
                   frmival.discrete.numerator;  // 分母除分子得到帧率
      //   std::cout << "    - Supported frame rate: " << fps << " fps" <<
      //   std::endl;
      if (std::abs(fps - 30.0) < 0.01)  // 考虑浮点数计算的误差
      {
        std::cout << "    - Supported frame rate: " << fps << " fps"
                  << std::endl;
        this->is30fpsSupport = true;
      }
    }
  }
  if (this->is30fpsSupport == false)
  {
    std::cerr
        << "Camera does not support 30fps frame rate at 640x480 resolution"
        << std::endl;
    return -1;
  }

  // 开始设置
  std::memset(&v4l2fmt, 0, sizeof(v4l2fmt));
  v4l2fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  v4l2fmt.fmt.pix.width = 640;
  v4l2fmt.fmt.pix.height = 480;
  v4l2fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
  v4l2fmt.fmt.pix.field = V4L2_FIELD_NONE;  // 设置为逐行扫描
  if (ioctl(camera_fd, VIDIOC_S_FMT, &v4l2fmt) < 0)
  {
    std::cerr << "Failed to set pixel format: " << std::strerror(errno)
              << std::endl;
    return -1;
  }
  // 检查返回值是否符合预期
  if (v4l2fmt.fmt.pix.width != 640 || v4l2fmt.fmt.pix.height != 480 ||
      v4l2fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG)
  {
    std::cerr << "Camera did not accept the requested format" << std::endl;
    return -1;
  }
  // 确认是否支持设置帧率
  std::memset(&streamparm, 0, sizeof(streamparm));
  streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(camera_fd, VIDIOC_G_PARM, &streamparm) < 0)
  {
    std::cerr << "Failed to get stream parameters: " << std::strerror(errno)
              << std::endl;
    return -1;
  }
  if (!(streamparm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME))
  {
    std::cerr << "Camera does not support setting frame rate" << std::endl;
    return -1;
  }
  // 支持帧率设置 开始设置帧率
  std::memset(&streamparm, 0, sizeof(streamparm));
  streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  streamparm.parm.capture.timeperframe.numerator = 1;     // 分子
  streamparm.parm.capture.timeperframe.denominator = 30;  // 分母
  if (ioctl(camera_fd, VIDIOC_S_PARM, &streamparm) < 0)
  {
    std::cerr << "Failed to set frame rate: " << std::strerror(errno)
              << std::endl;
    return -1;
  }
  // 检查设置结果 想做“二次确认”或兼容某些实现不规范的驱动，再额外 G_PARM
  // 一次也可以，但不是必需的
  if (streamparm.parm.capture.timeperframe.numerator == 0)
  {
    std::cerr << "Invalid frame rate returned by driver" << std::endl;
    return -1;
  }
  double fpsActual =
      static_cast<double>(streamparm.parm.capture.timeperframe.denominator) /
      streamparm.parm.capture.timeperframe.numerator;
  if (std::abs(fpsActual - 30.0) > 0.1)
  {
    std::cerr << "Camera did not accept the requested frame rate"
              << " (actual: " << fpsActual << " fps)" << std::endl;
    return -1;
  }
  std::cout << "Camera capabilities check passed.\nCamera Init Done "
            << std::endl;
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
    std::cerr << "Invalid exposure time: " << exposure_time_ms
              << " ms (must be >= 0)" << std::endl;
    return -1;
  }

  if (exposure_time_ms == 0)
  {
    v4l2_control auto_ctrl{};
    auto_ctrl.id = V4L2_CID_EXPOSURE_AUTO;
    auto_ctrl.value = V4L2_EXPOSURE_AUTO;
    if (ioctl(camera_fd, VIDIOC_S_CTRL, &auto_ctrl) < 0)
    {
      std::cerr << "Failed to set auto exposure mode: " << std::strerror(errno)
                << std::endl;
      return -1;
    }

    std::cout << "Exposure mode set to auto" << std::endl;
    return 0;
  }

  // Most UVC drivers use V4L2_CID_EXPOSURE_ABSOLUTE in 100us units.
  const int exposure_100us = exposure_time_ms * 10;

  v4l2_control auto_ctrl{};
  auto_ctrl.id = V4L2_CID_EXPOSURE_AUTO;
  auto_ctrl.value = V4L2_EXPOSURE_MANUAL;
  if (ioctl(camera_fd, VIDIOC_S_CTRL, &auto_ctrl) < 0)
  {
    std::cerr << "Failed to set manual exposure mode: " << std::strerror(errno)
              << std::endl;
    return -1;
  }

  v4l2_queryctrl query{};
  query.id = V4L2_CID_EXPOSURE_ABSOLUTE;
  if (ioctl(camera_fd, VIDIOC_QUERYCTRL, &query) < 0)
  {
    std::cerr << "Failed to query exposure range: " << std::strerror(errno)
              << std::endl;
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
              << " (supported " << (query.minimum / 10.0) << "-"
              << (query.maximum / 10.0) << " ms)" << std::endl;
    return -1;
  }

  v4l2_control exp_ctrl{};
  exp_ctrl.id = V4L2_CID_EXPOSURE_ABSOLUTE;
  exp_ctrl.value = exposure_100us;
  if (ioctl(camera_fd, VIDIOC_S_CTRL, &exp_ctrl) < 0)
  {
    std::cerr << "Failed to set exposure time: " << std::strerror(errno)
              << std::endl;
    return -1;
  }

  std::cout << "Exposure time set to " << exposure_time_ms << " ms"
            << std::endl;
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
    std::cerr << "Failed to close camera fd=" << camera_fd << ": "
              << std::strerror(errno) << std::endl;
    return -1;
  }

  camera_fd = -1;
  return 0;
}
