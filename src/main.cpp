#include <iostream>
#include <vector>

#include "v4l2Camera.h"

const std::string device = "/dev/video0";

int main()
{
    V4L2_Camera camera;
    if (camera.open_camera(device) != 0)
    {
        std::cerr << "Failed to open camera: " << device << std::endl;
        return 1;
    }

    if (camera.check_cameraCapabilities() != 0)
    {
        std::cerr << "Camera does not meet the required capabilities" << std::endl;
        return 1;
    }

    if (camera.init_camera_buffer() != 0)
    {
        std::cerr << "Failed to initialize camera buffers" << std::endl;
        return 1;
    }

    if (camera.set_exposure_time(100) != 0)
    {
        std::cerr << "Failed to set exposure time" << std::endl;
        return 1;
    }

    if (camera.capture_stream_switch(true) != 0)
    {
        std::cerr << "Failed to start streaming capture" << std::endl;
        return 1;
    }

    std::vector<unsigned char> frame(1024 * 1024);  // 1MB temporary frame buffer
    size_t                     frame_length = frame.size();
    if (camera.camera_read_frame(frame.data(), &frame_length) != 0)
    {
        std::cerr << "Failed to read a frame from the camera" << std::endl;
        return 1;
    }
    std::cout << "Read one frame, bytes: " << frame_length << std::endl;

    // 依赖析构函数完成资源的回收
    std::cout << "Camera opened and closed successfully" << std::endl;
    return 0;
}
