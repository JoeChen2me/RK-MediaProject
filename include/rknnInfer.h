#ifndef RKNN_INFER_H
#define RKNN_INFER_H

#include "rknn_api.h"
#include "pubDataType.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct NpuDetBox
{
    int   left   = 0;
    int   top    = 0;
    int   right  = 0;
    int   bottom = 0;
    float score  = 0.0F;
    int   cls_id = -1;
};

struct NpuDetResult
{
    int                          count  = 0;
    std::array<NpuDetBox, 128U>  boxes  = {};
    int64_t                      pts_us = -1;
    int64_t                      dts_us = -1;
};

struct RknnYoloConfig
{
    std::string model_path      = {};
    float       box_thresh      = 0.25F;
    float       nms_thresh      = 0.45F;
    int         draw_thickness  = 4;
    uint32_t    draw_color_rgba = 0x000000FFU;
    bool        enable_letterbox = true;
};

class RknnYoloInfer
{
   public:
    int  Init(const RknnYoloConfig& cfg);
    void Deinit();
    bool IsReady() const;
    int  InferAndDraw(const IO_FD_t* nv12_frame, NpuDetResult* out);

   private:
    bool                  initialized_ = false;
    RknnYoloConfig        cfg_         = {};
    rknn_context          rknn_ctx_    = 0;
    rknn_input_output_num io_num_      = {};
    std::vector<rknn_tensor_attr> input_attrs_  = {};
    std::vector<rknn_tensor_attr> output_attrs_ = {};
    int                          model_channel_ = 0;
    int                          model_width_   = 0;
    int                          model_height_  = 0;
    bool                         is_quant_      = false;
    std::vector<uint8_t>         input_rgb_     = {};
};

#endif
