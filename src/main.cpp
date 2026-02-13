#include <iostream>

#include "v4l2Camera.h"

int main() {
    V4L2_Camera camera;

    const char* device = "/dev/video0";
    if (camera.open_camera(device) != 0) {
        std::cerr << "Failed to open camera: " << device << std::endl;
        return 1;
    }

    if (camera.close_camera() != 0) {
        std::cerr << "Failed to close camera" << std::endl;
        return 1;
    }

    std::cout << "Camera opened and closed successfully" << std::endl;
    return 0;
}
