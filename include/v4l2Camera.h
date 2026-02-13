#ifndef V4L2_CAMERA_H
#define V4L2_CAMERA_H

#include <iostream>

class V4L2_Camera {
public:
    V4L2_Camera() {
        std::cout << "V4L2_Camera constructor called" << std::endl;
    }

    ~V4L2_Camera() {
        std::cout << "V4L2_Camera destructor called" << std::endl;
    }

    int open_camera(const char* device) {
        std::cout << "Opening camera device: " << device << std::endl;
        // Here you would add code to open the camera device and set camera_fd
        return 0; // Return 0 on success
    }

    int close_camera() {
        std::cout << "Closing camera device" << std::endl;
        // Here you would add code to close the camera device using camera_fd
        return 0; // Return 0 on success
    }

private:
    int camera_fd = -1; // File descriptor for the camera device
};

#endif // V4L2_CAMERA_H
