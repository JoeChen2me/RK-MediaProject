#include "rknnInfer.h"

int RknnYoloInfer::Init(const RknnYoloConfig& cfg)
{
    cfg_         = cfg;
    initialized_ = false;
    return 0;
}

void RknnYoloInfer::Deinit() { initialized_ = false; }

bool RknnYoloInfer::IsReady() const { return initialized_; }

int RknnYoloInfer::InferAndDraw(const IO_FD_t* nv12_frame, NpuDetResult* out)
{
    if (!nv12_frame || !out)
    {
        return -1;
    }
    return -1;
}
