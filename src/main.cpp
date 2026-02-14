#include <iostream>
#include <vector>
#include <csignal>
#include <errno.h>

#include "v4l2Camera.h"
#include "rkmpp.h"

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
    if (camera.camera_GlobalInit(device, 100) != 0)  // 全局初始化，设置曝光时间为100ms
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

    std::vector<unsigned char> frame(
        camera_params::kMaxFrameSize);  // 用于存储从摄像头读取的一帧数据的缓冲区
    size_t frame_length = frame.size();
    while (!should_exit)
    {
        if (camera.camera_read_frame(frame.data(), &frame_length, camera_params::kMaxFrameSize) !=
            0)
        {
            std::cerr << "Failed to read a frame from the camera" << std::endl;
            break;
        }
        std::cout << "Read one frame, bytes: " << frame_length << std::endl;
    }

    // 依赖析构函数完成资源的回收
    std::cout << "Camera opened and closed successfully" << std::endl;
    return 0;
}
