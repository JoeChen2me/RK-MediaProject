#include <csignal>
#include <errno.h>
#include <iostream>
#include <vector>

#include "rkmpp.h"
#include "v4l2Camera.h"

const std::string            device      = "/dev/video0";
static volatile sig_atomic_t should_exit = 0;  // 用于控制程序退出的标志

static void signal_handler(int signum)
{
    if (signum == SIGINT)
    {
        should_exit = 1;
    }
}

int main()
{
    // 注册 SIGINT 信号处理函数，以便在按下 Ctrl+C 时能够优雅地退出程序
    std::signal(SIGINT, signal_handler);

    V4L2_Camera camera;
    if (camera.camera_GlobalInit(device, 480) != 0)  // 全局初始化，设置曝光时间为 480ms
    {
        std::cerr << "Failed to initialize camera globally" << std::endl;
        return 1;
    }

    MppInstance mpp_instance;
    if (mpp_instance.MppInit() != 0)  // 初始化 MPP 实例
    {
        std::cerr << "Failed to initialize MPP instance" << std::endl;
        return 1;
    }
    if (mpp_instance.MppConfigWidthHeight(camera_params::kCaptureWidth,
                                          camera_params::kCaptureHeight) !=
        0)  // 配置 MPP 的宽高参数
    {
        std::cerr << "Failed to configure MPP width and height" << std::endl;
        return 1;
    }
    if (mpp_instance.MppAllocBuffer() != 0)
    {
        std::cerr << "Failed to allocate MPP buffer group" << std::endl;
        return 1;
    }

    std::vector<unsigned char> frame(
        camera_params::kMaxFrameSize);  // 用于存储从摄像头读取的一帧数据的缓冲区
    while (!should_exit)
    {
        size_t frame_length = frame.size();
        if (camera.camera_read_frame(frame.data(), &frame_length, camera_params::kMaxFrameSize) !=
            0)
        {
            std::cerr << "Failed to read a frame from the camera" << std::endl;
            break;
        }
        // 获取到一帧数据后，调用 MPP 解码函数进行解码
        if (mpp_instance.MppDecode(frame.data(), frame_length) != 0)
        {
            std::cerr << "Failed to decode the frame using MPP" << std::endl;
            break;
        }
        std::cout << "Frame captured and decoded successfully, length: " << frame_length << " bytes"
                  << std::endl;
    }

    // 依赖析构函数完成资源的回收
    std::cout << "Camera opened and closed successfully" << std::endl;
    return 0;
}
