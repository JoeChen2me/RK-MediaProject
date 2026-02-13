#include <iostream>

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

  if (camera.close_camera() != 0)
  {
    std::cerr << "Failed to close camera" << std::endl;
    return 1;
  }

  std::cout << "Camera opened and closed successfully" << std::endl;
  return 0;
}
