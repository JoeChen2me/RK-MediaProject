#include "v4l2Camera.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

V4L2_Camera::V4L2_Camera() {
    std::cout << "V4L2_Camera constructor called" << std::endl;
}

V4L2_Camera::~V4L2_Camera() {
    if (camera_fd >= 0) {
        close_camera();
    }
    std::cout << "V4L2_Camera destructor called" << std::endl;
}

int V4L2_Camera::open_camera(const char* device) {
    if (device == nullptr) {
        std::cerr << "Camera device path is null" << std::endl;
        return -1;
    }

    if (camera_fd >= 0) {
        std::cout << "Camera already opened: fd=" << camera_fd << std::endl;
        return 0;
    }

    std::cout << "Opening camera device: " << device << std::endl;
    camera_fd = open(device, O_RDWR);
    if (camera_fd < 0) {
        std::cerr << "Failed to open " << device << ": " << std::strerror(errno) << std::endl;
        return -1;
    }

    return 0;
}

int V4L2_Camera::close_camera() {
    if (camera_fd < 0) {
        std::cout << "Camera already closed" << std::endl;
        return 0;
    }

    std::cout << "Closing camera device: fd=" << camera_fd << std::endl;
    if (close(camera_fd) != 0) {
        std::cerr << "Failed to close camera fd=" << camera_fd << ": " << std::strerror(errno) << std::endl;
        return -1;
    }

    camera_fd = -1;
    return 0;
}
