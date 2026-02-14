#ifndef RKMPP_H
#define RKMPP_H

#include <rockchip/rk_mpi.h>
#include <rockchip/rk_mpi_cmd.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>

class MppInstance
{
   public:
    MppInstance();
    ~MppInstance();

    int MppInit();

   private:
    MppCtx  mpp_ctx = nullptr;  // MPP 上下文指针
    MppApi* mpp_api = nullptr;  // MPP API 指针
};

#endif  // RKMPP_H
