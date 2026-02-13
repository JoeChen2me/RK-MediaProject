#ifndef V4L2_CAMERA_H
#define V4L2_CAMERA_H

class V4L2_Camera {
public:
    V4L2_Camera();
    ~V4L2_Camera();

    int open_camera(const char* device);
    int close_camera();

private:
    int camera_fd = -1;
};

#endif // V4L2_CAMERA_H
