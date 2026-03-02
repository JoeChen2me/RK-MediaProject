#include "rknnInfer.h"

#include <rockchip/mpp_frame.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <set>
#include <vector>

#include <im2d.h>
#include <im2d_buffer.h>

namespace
{
constexpr int   kObjClassNum = 80;
constexpr int   kObjMaxNum   = 128;
constexpr float kEps         = 1e-6F;

struct LetterboxInfo
{
    int   x_pad = 0;
    int   y_pad = 0;
    float scale = 1.0F;
};

struct DecodeContext
{
    std::vector<float> boxes;
    std::vector<float> probs;
    std::vector<int>   class_ids;
};

inline int ClampInt(int val, int min_val, int max_val)
{
    return (val < min_val) ? min_val : ((val > max_val) ? max_val : val);
}

inline float ClampFloat(float val, float min_val, float max_val)
{
    return (val < min_val) ? min_val : ((val > max_val) ? max_val : val);
}

bool LoadBinaryFile(const std::string& file_path, std::vector<uint8_t>* out)
{
    if (!out)
    {
        return false;
    }

    std::ifstream ifs(file_path, std::ios::binary | std::ios::ate);
    if (!ifs)
    {
        std::cerr << "Failed to open model file: " << file_path << std::endl;
        return false;
    }

    const std::streamsize size = ifs.tellg();
    if (size <= 0)
    {
        std::cerr << "Model file is empty: " << file_path << std::endl;
        return false;
    }

    out->resize(static_cast<size_t>(size));
    ifs.seekg(0, std::ios::beg);
    if (!ifs.read(reinterpret_cast<char*>(out->data()), size))
    {
        std::cerr << "Failed to read model file: " << file_path << std::endl;
        return false;
    }

    return true;
}

float CalculateOverlap(float xmin0, float ymin0, float xmax0, float ymax0, float xmin1, float ymin1,
                       float xmax1, float ymax1)
{
    const float w = std::max(0.0F, std::min(xmax0, xmax1) - std::max(xmin0, xmin1) + 1.0F);
    const float h = std::max(0.0F, std::min(ymax0, ymax1) - std::max(ymin0, ymin1) + 1.0F);
    const float i = w * h;
    const float u = (xmax0 - xmin0 + 1.0F) * (ymax0 - ymin0 + 1.0F) +
                    (xmax1 - xmin1 + 1.0F) * (ymax1 - ymin1 + 1.0F) - i;
    return (u <= 0.0F) ? 0.0F : (i / u);
}

int8_t QntF32ToAffine(float f32, int32_t zp, float scale)
{
    const float dst_val = (f32 / scale) + static_cast<float>(zp);
    return static_cast<int8_t>(ClampFloat(dst_val, -128.0F, 127.0F));
}

uint8_t QntF32ToAffineU8(float f32, int32_t zp, float scale)
{
    const float dst_val = (f32 / scale) + static_cast<float>(zp);
    return static_cast<uint8_t>(ClampFloat(dst_val, 0.0F, 255.0F));
}

float DeqntAffineToF32(int8_t qnt, int32_t zp, float scale)
{
    return (static_cast<float>(qnt) - static_cast<float>(zp)) * scale;
}

float DeqntAffineU8ToF32(uint8_t qnt, int32_t zp, float scale)
{
    return (static_cast<float>(qnt) - static_cast<float>(zp)) * scale;
}

void ComputeDfl(const float* tensor, int dfl_len, float* box)
{
    if (!tensor || !box || dfl_len <= 0)
    {
        return;
    }

    for (int b = 0; b < 4; ++b)
    {
        std::vector<float> exp_t(static_cast<size_t>(dfl_len), 0.0F);
        float              exp_sum = 0.0F;
        float              acc_sum = 0.0F;
        for (int i = 0; i < dfl_len; ++i)
        {
            exp_t[static_cast<size_t>(i)] = std::exp(tensor[i + b * dfl_len]);
            exp_sum += exp_t[static_cast<size_t>(i)];
        }
        const float inv_sum = (exp_sum > kEps) ? (1.0F / exp_sum) : 0.0F;
        for (int i = 0; i < dfl_len; ++i)
        {
            acc_sum += exp_t[static_cast<size_t>(i)] * inv_sum * static_cast<float>(i);
        }
        box[b] = acc_sum;
    }
}

int ProcessI8(const int8_t* box_tensor, int32_t box_zp, float box_scale, const int8_t* score_tensor,
              int32_t score_zp, float score_scale, const int8_t* score_sum_tensor, int32_t score_sum_zp,
              float score_sum_scale, int grid_h, int grid_w, int stride, int dfl_len,
              DecodeContext* ctx, float threshold)
{
    if (!box_tensor || !score_tensor || !ctx)
    {
        return 0;
    }

    int       valid_count         = 0;
    const int grid_len            = grid_h * grid_w;
    const int8_t score_thres_i8   = QntF32ToAffine(threshold, score_zp, score_scale);
    const int8_t score_sum_thres  = QntF32ToAffine(threshold, score_sum_zp, score_sum_scale);

    for (int i = 0; i < grid_h; ++i)
    {
        for (int j = 0; j < grid_w; ++j)
        {
            int offset       = i * grid_w + j;
            int max_class_id = -1;

            if (score_sum_tensor && score_sum_tensor[offset] < score_sum_thres)
            {
                continue;
            }

            int8_t max_score = static_cast<int8_t>(-score_zp);
            for (int c = 0; c < kObjClassNum; ++c)
            {
                if ((score_tensor[offset] > score_thres_i8) && (score_tensor[offset] > max_score))
                {
                    max_score    = score_tensor[offset];
                    max_class_id = c;
                }
                offset += grid_len;
            }

            if (max_score <= score_thres_i8)
            {
                continue;
            }

            offset = i * grid_w + j;
            float box[4] = {0.0F, 0.0F, 0.0F, 0.0F};
            std::vector<float> before_dfl(static_cast<size_t>(dfl_len * 4), 0.0F);
            for (int k = 0; k < dfl_len * 4; ++k)
            {
                before_dfl[static_cast<size_t>(k)] = DeqntAffineToF32(box_tensor[offset], box_zp, box_scale);
                offset += grid_len;
            }
            ComputeDfl(before_dfl.data(), dfl_len, box);

            const float x1 = (-box[0] + static_cast<float>(j) + 0.5F) * static_cast<float>(stride);
            const float y1 = (-box[1] + static_cast<float>(i) + 0.5F) * static_cast<float>(stride);
            const float x2 = (box[2] + static_cast<float>(j) + 0.5F) * static_cast<float>(stride);
            const float y2 = (box[3] + static_cast<float>(i) + 0.5F) * static_cast<float>(stride);

            ctx->boxes.push_back(x1);
            ctx->boxes.push_back(y1);
            ctx->boxes.push_back(x2 - x1);
            ctx->boxes.push_back(y2 - y1);
            ctx->probs.push_back(DeqntAffineToF32(max_score, score_zp, score_scale));
            ctx->class_ids.push_back(max_class_id);
            valid_count++;
        }
    }
    return valid_count;
}

int ProcessU8(const uint8_t* box_tensor, int32_t box_zp, float box_scale, const uint8_t* score_tensor,
              int32_t score_zp, float score_scale, const uint8_t* score_sum_tensor, int32_t score_sum_zp,
              float score_sum_scale, int grid_h, int grid_w, int stride, int dfl_len,
              DecodeContext* ctx, float threshold)
{
    if (!box_tensor || !score_tensor || !ctx)
    {
        return 0;
    }

    int       valid_count         = 0;
    const int grid_len            = grid_h * grid_w;
    const uint8_t score_thres_u8  = QntF32ToAffineU8(threshold, score_zp, score_scale);
    const uint8_t score_sum_thres = QntF32ToAffineU8(threshold, score_sum_zp, score_sum_scale);

    for (int i = 0; i < grid_h; ++i)
    {
        for (int j = 0; j < grid_w; ++j)
        {
            int offset       = i * grid_w + j;
            int max_class_id = -1;

            if (score_sum_tensor && score_sum_tensor[offset] < score_sum_thres)
            {
                continue;
            }

            uint8_t max_score = 0;
            for (int c = 0; c < kObjClassNum; ++c)
            {
                if ((score_tensor[offset] > score_thres_u8) && (score_tensor[offset] > max_score))
                {
                    max_score    = score_tensor[offset];
                    max_class_id = c;
                }
                offset += grid_len;
            }

            if (max_score <= score_thres_u8)
            {
                continue;
            }

            offset = i * grid_w + j;
            float box[4] = {0.0F, 0.0F, 0.0F, 0.0F};
            std::vector<float> before_dfl(static_cast<size_t>(dfl_len * 4), 0.0F);
            for (int k = 0; k < dfl_len * 4; ++k)
            {
                before_dfl[static_cast<size_t>(k)] = DeqntAffineU8ToF32(box_tensor[offset], box_zp, box_scale);
                offset += grid_len;
            }
            ComputeDfl(before_dfl.data(), dfl_len, box);

            const float x1 = (-box[0] + static_cast<float>(j) + 0.5F) * static_cast<float>(stride);
            const float y1 = (-box[1] + static_cast<float>(i) + 0.5F) * static_cast<float>(stride);
            const float x2 = (box[2] + static_cast<float>(j) + 0.5F) * static_cast<float>(stride);
            const float y2 = (box[3] + static_cast<float>(i) + 0.5F) * static_cast<float>(stride);

            ctx->boxes.push_back(x1);
            ctx->boxes.push_back(y1);
            ctx->boxes.push_back(x2 - x1);
            ctx->boxes.push_back(y2 - y1);
            ctx->probs.push_back(DeqntAffineU8ToF32(max_score, score_zp, score_scale));
            ctx->class_ids.push_back(max_class_id);
            valid_count++;
        }
    }
    return valid_count;
}

int ProcessFp32(const float* box_tensor, const float* score_tensor, const float* score_sum_tensor,
                int grid_h, int grid_w, int stride, int dfl_len, DecodeContext* ctx, float threshold)
{
    if (!box_tensor || !score_tensor || !ctx)
    {
        return 0;
    }

    int       valid_count = 0;
    const int grid_len    = grid_h * grid_w;

    for (int i = 0; i < grid_h; ++i)
    {
        for (int j = 0; j < grid_w; ++j)
        {
            int offset       = i * grid_w + j;
            int max_class_id = -1;

            if (score_sum_tensor && score_sum_tensor[offset] < threshold)
            {
                continue;
            }

            float max_score = 0.0F;
            for (int c = 0; c < kObjClassNum; ++c)
            {
                if ((score_tensor[offset] > threshold) && (score_tensor[offset] > max_score))
                {
                    max_score    = score_tensor[offset];
                    max_class_id = c;
                }
                offset += grid_len;
            }

            if (max_score <= threshold)
            {
                continue;
            }

            offset = i * grid_w + j;
            float box[4] = {0.0F, 0.0F, 0.0F, 0.0F};
            std::vector<float> before_dfl(static_cast<size_t>(dfl_len * 4), 0.0F);
            for (int k = 0; k < dfl_len * 4; ++k)
            {
                before_dfl[static_cast<size_t>(k)] = box_tensor[offset];
                offset += grid_len;
            }
            ComputeDfl(before_dfl.data(), dfl_len, box);

            const float x1 = (-box[0] + static_cast<float>(j) + 0.5F) * static_cast<float>(stride);
            const float y1 = (-box[1] + static_cast<float>(i) + 0.5F) * static_cast<float>(stride);
            const float x2 = (box[2] + static_cast<float>(j) + 0.5F) * static_cast<float>(stride);
            const float y2 = (box[3] + static_cast<float>(i) + 0.5F) * static_cast<float>(stride);

            ctx->boxes.push_back(x1);
            ctx->boxes.push_back(y1);
            ctx->boxes.push_back(x2 - x1);
            ctx->boxes.push_back(y2 - y1);
            ctx->probs.push_back(max_score);
            ctx->class_ids.push_back(max_class_id);
            valid_count++;
        }
    }

    return valid_count;
}

void DumpTensorAttr(const rknn_tensor_attr& attr)
{
    std::cerr << "  index=" << attr.index << ", name=" << attr.name << ", n_dims=" << attr.n_dims
              << ", dims=[" << attr.dims[0] << "," << attr.dims[1] << "," << attr.dims[2] << ","
              << attr.dims[3] << "]"
              << ", n_elems=" << attr.n_elems << ", size=" << attr.size << ", fmt=" << attr.fmt
              << ", type=" << attr.type << ", qnt_type=" << attr.qnt_type << ", zp=" << attr.zp
              << ", scale=" << attr.scale << std::endl;
}

bool IsNv12Format(const IO_FD_t* frame)
{
    if (!frame)
    {
        return false;
    }

    uint32_t fmt = frame->format;
    if (fmt == 0)
    {
        fmt = static_cast<uint32_t>(MPP_FMT_YUV420SP);
    }
    fmt &= MPP_FRAME_FMT_MASK;
    return fmt == static_cast<uint32_t>(MPP_FMT_YUV420SP);
}

int BuildSourceNv12RgaBuffer(const IO_FD_t* frame, rga_buffer_t* out)
{
    if (!frame || !out)
    {
        return -1;
    }

    if (frame->fd >= 0)
    {
        *out = wrapbuffer_fd(frame->fd, static_cast<int>(frame->width), static_cast<int>(frame->height),
                             RK_FORMAT_YCbCr_420_SP, static_cast<int>(frame->hor_stride),
                             static_cast<int>(frame->ver_stride));
        return 0;
    }

    if (frame->base)
    {
        *out = wrapbuffer_virtualaddr_t(frame->base, static_cast<int>(frame->width),
                                        static_cast<int>(frame->height), RK_FORMAT_YCbCr_420_SP,
                                        static_cast<int>(frame->hor_stride),
                                        static_cast<int>(frame->ver_stride));
        return 0;
    }

    return -1;
}

int PreprocessNv12ToRgbLetterbox(const IO_FD_t* frame, int model_w, int model_h,
                                 std::vector<uint8_t>* rgb_out, LetterboxInfo* out_lb)
{
    if (!frame || !rgb_out || !out_lb)
    {
        return -1;
    }
    if (!IsNv12Format(frame) || frame->width == 0 || frame->height == 0 || frame->hor_stride == 0 ||
        frame->ver_stride == 0)
    {
        std::cerr << "Unsupported frame metadata for NPU preprocess" << std::endl;
        return -1;
    }

    rgb_out->assign(static_cast<size_t>(model_w * model_h * 3), 114U);

    const float scale_w = static_cast<float>(model_w) / static_cast<float>(frame->width);
    const float scale_h = static_cast<float>(model_h) / static_cast<float>(frame->height);
    const float scale   = std::min(scale_w, scale_h);

    int resized_w = std::max(1, static_cast<int>(std::round(static_cast<float>(frame->width) * scale)));
    int resized_h = std::max(1, static_cast<int>(std::round(static_cast<float>(frame->height) * scale)));
    resized_w     = std::min(resized_w, model_w);
    resized_h     = std::min(resized_h, model_h);

    const int x_pad = (model_w - resized_w) / 2;
    const int y_pad = (model_h - resized_h) / 2;

    rga_buffer_t src = {};
    if (BuildSourceNv12RgaBuffer(frame, &src) != 0)
    {
        std::cerr << "Failed to build source buffer for preprocess" << std::endl;
        return -1;
    }

    rga_buffer_t dst = wrapbuffer_virtualaddr_t(rgb_out->data(), model_w, model_h, RK_FORMAT_RGB_888,
                                                model_w, model_h);
    rga_buffer_t pat = {};

    im_rect srect = {0, 0, static_cast<int>(frame->width), static_cast<int>(frame->height)};
    im_rect drect = {x_pad, y_pad, resized_w, resized_h};
    im_rect prect = {0, 0, 0, 0};

    const IM_STATUS run_ret = improcess(src, dst, pat, srect, drect, prect, 0);
    if (run_ret != IM_STATUS_SUCCESS)
    {
        std::cerr << "RGA preprocess improcess failed: " << imStrError_t(run_ret) << std::endl;
        return -1;
    }

    out_lb->x_pad = x_pad;
    out_lb->y_pad = y_pad;
    out_lb->scale = (scale > kEps) ? scale : 1.0F;
    return 0;
}

int DecodeYolov8Results(const rknn_input_output_num& io_num, const std::vector<rknn_tensor_attr>& out_attrs,
                        rknn_output* outputs, const LetterboxInfo& lb, int model_w, int model_h,
                        NpuDetResult* out)
{
    if (!outputs || !out || io_num.n_output < 6 || out_attrs.empty())
    {
        return -1;
    }

    DecodeContext ctx;
    int           valid_count       = 0;
    const int     output_per_branch = static_cast<int>(io_num.n_output) / 3;
    if (output_per_branch < 2)
    {
        return -1;
    }

    int dfl_len = static_cast<int>(out_attrs[0].dims[1] / 4U);

    for (int i = 0; i < 3; ++i)
    {
        const int box_idx   = i * output_per_branch;
        const int score_idx = i * output_per_branch + 1;
        const int sum_idx   = i * output_per_branch + 2;

        const int grid_h = static_cast<int>(out_attrs[box_idx].dims[2]);
        const int grid_w = static_cast<int>(out_attrs[box_idx].dims[3]);
        if (grid_h <= 0 || grid_w <= 0)
        {
            continue;
        }
        const int stride = model_h / grid_h;

        void*   score_sum       = nullptr;
        int32_t score_sum_zp    = 0;
        float   score_sum_scale = 1.0F;
        if (output_per_branch >= 3)
        {
            score_sum       = outputs[sum_idx].buf;
            score_sum_zp    = out_attrs[sum_idx].zp;
            score_sum_scale = out_attrs[sum_idx].scale;
        }

        if (out_attrs[box_idx].type == RKNN_TENSOR_INT8)
        {
            valid_count += ProcessI8(static_cast<const int8_t*>(outputs[box_idx].buf), out_attrs[box_idx].zp,
                                     out_attrs[box_idx].scale,
                                     static_cast<const int8_t*>(outputs[score_idx].buf),
                                     out_attrs[score_idx].zp, out_attrs[score_idx].scale,
                                     static_cast<const int8_t*>(score_sum), score_sum_zp, score_sum_scale,
                                     grid_h, grid_w, stride, dfl_len, &ctx, 0.25F);
        }
        else if (out_attrs[box_idx].type == RKNN_TENSOR_UINT8)
        {
            valid_count += ProcessU8(static_cast<const uint8_t*>(outputs[box_idx].buf),
                                     out_attrs[box_idx].zp, out_attrs[box_idx].scale,
                                     static_cast<const uint8_t*>(outputs[score_idx].buf),
                                     out_attrs[score_idx].zp, out_attrs[score_idx].scale,
                                     static_cast<const uint8_t*>(score_sum), score_sum_zp, score_sum_scale,
                                     grid_h, grid_w, stride, dfl_len, &ctx, 0.25F);
        }
        else
        {
            valid_count += ProcessFp32(static_cast<const float*>(outputs[box_idx].buf),
                                       static_cast<const float*>(outputs[score_idx].buf),
                                       static_cast<const float*>(score_sum), grid_h, grid_w, stride,
                                       dfl_len, &ctx, 0.25F);
        }
    }

    if (valid_count <= 0 || ctx.probs.empty())
    {
        return 0;
    }

    std::vector<int> order(static_cast<size_t>(valid_count));
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return ctx.probs[static_cast<size_t>(a)] > ctx.probs[static_cast<size_t>(b)]; });

    std::vector<bool> suppressed(static_cast<size_t>(valid_count), false);
    std::set<int>     class_set(ctx.class_ids.begin(), ctx.class_ids.end());

    for (int cls : class_set)
    {
        for (size_t i = 0; i < order.size(); ++i)
        {
            const int idx_i = order[i];
            if (suppressed[static_cast<size_t>(idx_i)] || ctx.class_ids[static_cast<size_t>(idx_i)] != cls)
            {
                continue;
            }

            const float xmin0 = ctx.boxes[static_cast<size_t>(idx_i) * 4U + 0U];
            const float ymin0 = ctx.boxes[static_cast<size_t>(idx_i) * 4U + 1U];
            const float xmax0 = xmin0 + ctx.boxes[static_cast<size_t>(idx_i) * 4U + 2U];
            const float ymax0 = ymin0 + ctx.boxes[static_cast<size_t>(idx_i) * 4U + 3U];

            for (size_t j = i + 1; j < order.size(); ++j)
            {
                const int idx_j = order[j];
                if (suppressed[static_cast<size_t>(idx_j)] || ctx.class_ids[static_cast<size_t>(idx_j)] != cls)
                {
                    continue;
                }

                const float xmin1 = ctx.boxes[static_cast<size_t>(idx_j) * 4U + 0U];
                const float ymin1 = ctx.boxes[static_cast<size_t>(idx_j) * 4U + 1U];
                const float xmax1 = xmin1 + ctx.boxes[static_cast<size_t>(idx_j) * 4U + 2U];
                const float ymax1 = ymin1 + ctx.boxes[static_cast<size_t>(idx_j) * 4U + 3U];
                if (CalculateOverlap(xmin0, ymin0, xmax0, ymax0, xmin1, ymin1, xmax1, ymax1) > 0.45F)
                {
                    suppressed[static_cast<size_t>(idx_j)] = true;
                }
            }
        }
    }

    out->count = 0;
    for (int idx : order)
    {
        if (out->count >= kObjMaxNum || suppressed[static_cast<size_t>(idx)])
        {
            continue;
        }

        const float box_x = ctx.boxes[static_cast<size_t>(idx) * 4U + 0U];
        const float box_y = ctx.boxes[static_cast<size_t>(idx) * 4U + 1U];
        const float box_w = ctx.boxes[static_cast<size_t>(idx) * 4U + 2U];
        const float box_h = ctx.boxes[static_cast<size_t>(idx) * 4U + 3U];

        const float x1 = (box_x - static_cast<float>(lb.x_pad)) / lb.scale;
        const float y1 = (box_y - static_cast<float>(lb.y_pad)) / lb.scale;
        const float x2 = (box_x + box_w - static_cast<float>(lb.x_pad)) / lb.scale;
        const float y2 = (box_y + box_h - static_cast<float>(lb.y_pad)) / lb.scale;

        NpuDetBox& dst = out->boxes[static_cast<size_t>(out->count)];
        dst.left       = ClampInt(static_cast<int>(std::round(x1)), 0, model_w - 1);
        dst.top        = ClampInt(static_cast<int>(std::round(y1)), 0, model_h - 1);
        dst.right      = ClampInt(static_cast<int>(std::round(x2)), 0, model_w - 1);
        dst.bottom     = ClampInt(static_cast<int>(std::round(y2)), 0, model_h - 1);
        dst.score      = ctx.probs[static_cast<size_t>(idx)];
        dst.cls_id     = ctx.class_ids[static_cast<size_t>(idx)];

        if (dst.right > dst.left && dst.bottom > dst.top)
        {
            out->count++;
        }
    }

    return 0;
}

int DrawBoxesWithRga(const IO_FD_t* frame, const NpuDetResult* det, uint32_t color_rgba, int thickness)
{
    if (!frame || !det)
    {
        return -1;
    }
    if (det->count <= 0)
    {
        return 0;
    }
    if (!IsNv12Format(frame))
    {
        return -1;
    }

    std::vector<im_rect> rects;
    rects.reserve(static_cast<size_t>(det->count));

    for (int i = 0; i < det->count; ++i)
    {
        const NpuDetBox& box = det->boxes[static_cast<size_t>(i)];
        const int left   = ClampInt(box.left, 0, static_cast<int>(frame->width) - 1);
        const int top    = ClampInt(box.top, 0, static_cast<int>(frame->height) - 1);
        const int right  = ClampInt(box.right, 0, static_cast<int>(frame->width) - 1);
        const int bottom = ClampInt(box.bottom, 0, static_cast<int>(frame->height) - 1);
        const int width  = right - left + 1;
        const int height = bottom - top + 1;
        if (width < 2 || height < 2)
        {
            continue;
        }
        im_rect rect = {left, top, width, height};
        rects.push_back(rect);
    }

    if (rects.empty())
    {
        return 0;
    }

    rga_buffer_t dst = {};
    if (BuildSourceNv12RgaBuffer(frame, &dst) != 0)
    {
        return -1;
    }

    const int       line_width = (thickness < 2 && thickness >= 0) ? 2 : thickness;
    const IM_STATUS draw_ret   =
        imrectangleArray(dst, rects.data(), static_cast<int>(rects.size()), color_rgba, line_width);
    if (draw_ret != IM_STATUS_SUCCESS)
    {
        std::cerr << "RGA draw rectangle failed: " << imStrError_t(draw_ret) << std::endl;
        return -1;
    }

    return 0;
}
}  // namespace

int RknnYoloInfer::Init(const RknnYoloConfig& cfg)
{
    Deinit();

    if (cfg.model_path.empty())
    {
        std::cerr << "RKNN model path is empty" << std::endl;
        return -1;
    }

    std::vector<uint8_t> model_data;
    if (!LoadBinaryFile(cfg.model_path, &model_data))
    {
        return -1;
    }

    int ret = rknn_init(&rknn_ctx_, model_data.data(), model_data.size(), 0, nullptr);
    if (ret < 0)
    {
        std::cerr << "rknn_init failed, ret=" << ret << std::endl;
        rknn_ctx_ = 0;
        return -1;
    }

    rknn_sdk_version version = {};
    ret = rknn_query(rknn_ctx_, RKNN_QUERY_SDK_VERSION, &version, sizeof(version));
    if (ret == RKNN_SUCC)
    {
        std::cout << "[RKNN] api=" << version.api_version << ", drv=" << version.drv_version
                  << std::endl;
    }

    ret = rknn_query(rknn_ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num_, sizeof(io_num_));
    if (ret != RKNN_SUCC)
    {
        std::cerr << "rknn_query(RKNN_QUERY_IN_OUT_NUM) failed, ret=" << ret << std::endl;
        Deinit();
        return -1;
    }

    if (io_num_.n_input == 0 || io_num_.n_output == 0)
    {
        std::cerr << "Invalid model io_num: input=" << io_num_.n_input
                  << ", output=" << io_num_.n_output << std::endl;
        Deinit();
        return -1;
    }

    input_attrs_.resize(io_num_.n_input);
    output_attrs_.resize(io_num_.n_output);

    for (uint32_t i = 0; i < io_num_.n_input; ++i)
    {
        input_attrs_[i]       = {};
        input_attrs_[i].index = i;
        ret = rknn_query(rknn_ctx_, RKNN_QUERY_INPUT_ATTR, &input_attrs_[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC)
        {
            std::cerr << "rknn_query(input attr) failed index=" << i << ", ret=" << ret
                      << std::endl;
            Deinit();
            return -1;
        }
        DumpTensorAttr(input_attrs_[i]);
    }

    for (uint32_t i = 0; i < io_num_.n_output; ++i)
    {
        output_attrs_[i]       = {};
        output_attrs_[i].index = i;
        ret =
            rknn_query(rknn_ctx_, RKNN_QUERY_OUTPUT_ATTR, &output_attrs_[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC)
        {
            std::cerr << "rknn_query(output attr) failed index=" << i << ", ret=" << ret
                      << std::endl;
            Deinit();
            return -1;
        }
        DumpTensorAttr(output_attrs_[i]);
    }

    if (input_attrs_[0].fmt == RKNN_TENSOR_NCHW)
    {
        model_channel_ = static_cast<int>(input_attrs_[0].dims[1]);
        model_height_  = static_cast<int>(input_attrs_[0].dims[2]);
        model_width_   = static_cast<int>(input_attrs_[0].dims[3]);
    }
    else
    {
        model_height_  = static_cast<int>(input_attrs_[0].dims[1]);
        model_width_   = static_cast<int>(input_attrs_[0].dims[2]);
        model_channel_ = static_cast<int>(input_attrs_[0].dims[3]);
    }

    if (model_width_ <= 0 || model_height_ <= 0 || model_channel_ != 3)
    {
        std::cerr << "Unsupported model input shape: " << model_width_ << "x" << model_height_
                  << "x" << model_channel_ << std::endl;
        Deinit();
        return -1;
    }

    is_quant_ = (output_attrs_[0].qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC) &&
                (output_attrs_[0].type == RKNN_TENSOR_INT8 || output_attrs_[0].type == RKNN_TENSOR_UINT8);

    input_rgb_.resize(static_cast<size_t>(model_width_ * model_height_ * model_channel_), 0U);
    cfg_         = cfg;
    initialized_ = true;

    std::cout << "[RKNN] model loaded, input=" << model_width_ << "x" << model_height_
              << ", quant=" << (is_quant_ ? "yes" : "no") << std::endl;

    return 0;
}

void RknnYoloInfer::Deinit()
{
    if (rknn_ctx_ != 0)
    {
        (void)rknn_destroy(rknn_ctx_);
        rknn_ctx_ = 0;
    }

    io_num_       = {};
    input_attrs_.clear();
    output_attrs_.clear();
    model_channel_ = 0;
    model_width_   = 0;
    model_height_  = 0;
    is_quant_      = false;
    input_rgb_.clear();
    initialized_ = false;
}

bool RknnYoloInfer::IsReady() const { return initialized_; }

int RknnYoloInfer::InferAndDraw(const IO_FD_t* nv12_frame, NpuDetResult* out)
{
    if (!out)
    {
        return -1;
    }

    out->count  = 0;
    out->pts_us = (nv12_frame ? nv12_frame->pts_us : -1);
    out->dts_us = (nv12_frame ? nv12_frame->dts_us : -1);

    if (!initialized_ || !nv12_frame)
    {
        return -1;
    }

    LetterboxInfo lb = {};
    if (PreprocessNv12ToRgbLetterbox(nv12_frame, model_width_, model_height_, &input_rgb_, &lb) != 0)
    {
        return -1;
    }

    std::vector<rknn_input> inputs(io_num_.n_input);
    inputs[0]               = {};
    inputs[0].index         = 0;
    inputs[0].buf           = input_rgb_.data();
    inputs[0].size          = static_cast<uint32_t>(input_rgb_.size());
    inputs[0].pass_through  = 0;
    inputs[0].type          = RKNN_TENSOR_UINT8;
    inputs[0].fmt           = RKNN_TENSOR_NHWC;

    int ret = rknn_inputs_set(rknn_ctx_, io_num_.n_input, inputs.data());
    if (ret != RKNN_SUCC)
    {
        std::cerr << "rknn_inputs_set failed, ret=" << ret << std::endl;
        return -1;
    }

    ret = rknn_run(rknn_ctx_, nullptr);
    if (ret != RKNN_SUCC)
    {
        std::cerr << "rknn_run failed, ret=" << ret << std::endl;
        return -1;
    }

    std::vector<rknn_output> outputs(io_num_.n_output);
    for (uint32_t i = 0; i < io_num_.n_output; ++i)
    {
        outputs[i]         = {};
        outputs[i].index   = i;
        const bool quant_tensor =
            (output_attrs_[i].qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC) &&
            (output_attrs_[i].type == RKNN_TENSOR_INT8 || output_attrs_[i].type == RKNN_TENSOR_UINT8);
        outputs[i].want_float = quant_tensor ? 0 : 1;
    }

    ret = rknn_outputs_get(rknn_ctx_, io_num_.n_output, outputs.data(), nullptr);
    if (ret != RKNN_SUCC)
    {
        std::cerr << "rknn_outputs_get failed, ret=" << ret << std::endl;
        return -1;
    }

    const int decode_ret =
        DecodeYolov8Results(io_num_, output_attrs_, outputs.data(), lb, model_width_, model_height_, out);

    (void)rknn_outputs_release(rknn_ctx_, io_num_.n_output, outputs.data());

    if (decode_ret != 0)
    {
        return -1;
    }

    if (DrawBoxesWithRga(nv12_frame, out, cfg_.draw_color_rgba, cfg_.draw_thickness) != 0)
    {
        return -1;
    }

    return 0;
}
