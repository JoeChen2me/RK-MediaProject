#ifndef V4L2_CAMERA_H
#define V4L2_CAMERA_H

#include <linux/videodev2.h>

#include <string>

class V4L2_Camera
{
 public:
  V4L2_Camera();
  ~V4L2_Camera();
  // int Camera_Init(const char* device);
  int open_camera(const char* device);  // 打开摄像头
  int open_camera(
      const std::string& device);  // 重载函数，接受 std::string 参数
  int check_cameraCapabilities();  // 检查摄像头能力
  /**
   * @brief 设置曝光模式/曝光时间。
   * @param exposure_time_ms 曝光时间（毫秒）；传入 0 表示切换为自动曝光。
   * @return 0 成功，-1 失败。
   */
  int set_exposure_time(int exposure_time_ms);
  int close_camera();

 private:
  int camera_fd = -1;
  struct v4l2_capability cap;
  struct v4l2_fmtdesc fmtdesc;
  struct v4l2_frmsizeenum frmsize;
  struct v4l2_frmivalenum frmival;
  struct v4l2_format v4l2fmt;
  struct v4l2_streamparm streamparm;
  bool isMJPEGSupport = false;
  bool is640x480Support = false;
  bool is30fpsSupport = false;
};

struct v4l2CapList
{
  bool isMJPEGSupport = false;
  bool isYUYVSupport = false;
};

#endif  // V4L2_CAMERA_H
