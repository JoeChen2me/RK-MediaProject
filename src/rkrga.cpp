#include "rkrga.h"

#include <fcntl.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>

#include <im2d.h>
#include <im2d_buffer.h>
#include <im2d_common.h>
#include <im2d_single.h>
#include <rga.h>

namespace
{
constexpr int kRgaFormatNv12 = RK_FORMAT_YCbCr_420_SP;

inline uint32_t Align16(uint32_t value) { return (value + 15u) & ~15u; }

int BuildRgaBuffer(const MppOutputFD* buffer, uint32_t width, uint32_t height, uint32_t h_stride,
                   uint32_t v_stride, rga_buffer_t* out, rga_buffer_handle_t* handle)
{
    if (!buffer || !out || !handle || width == 0 || height == 0 || h_stride == 0 || v_stride == 0)
    {
        return -1;
    }

    *handle = 0;
    if (buffer->fd >= 0 && buffer->size > 0)
    {
        *handle = importbuffer_fd(buffer->fd, static_cast<int>(buffer->size));
        if (*handle)
        {
            *out = wrapbuffer_handle(*handle, static_cast<int>(width), static_cast<int>(height),
                                     kRgaFormatNv12, static_cast<int>(h_stride),
                                     static_cast<int>(v_stride));
            return 0;
        }
    }

    if (buffer->base && buffer->size > 0)
    {
        *out = wrapbuffer_virtualaddr_t(buffer->base, static_cast<int>(width),
                                        static_cast<int>(height), static_cast<int>(h_stride),
                                        static_cast<int>(v_stride), kRgaFormatNv12);
        return 0;
    }

    return -1;
}

void ReleaseRgaHandle(rga_buffer_handle_t& handle)
{
    if (handle)
    {
        (void)releasebuffer_handle(handle);
        handle = 0;
    }
}
}  // namespace

RgaInstance::RgaInstance() = default;

RgaInstance::~RgaInstance() = default;

int RgaInstance::RgaInit()
{
    IM_STATUS ret = imcheckHeader();
    if (ret != IM_STATUS_SUCCESS)
    {
        std::cerr << "RGA header/runtime check failed: " << imStrError_t(ret) << std::endl;
        return -1;
    }

    initialized_ = true;
    return 0;
}

int RgaInstance::AllocDmaBufFD(MppOutputFD* output, size_t size, const char* heap_path)
{
    if (!output || !heap_path || size == 0)
    {
        return -1;
    }

    ReleaseDmaBufFD(output);

    int heap = open(heap_path, O_RDWR | O_CLOEXEC);
    if (heap < 0)
    {
        std::cerr << "Failed to open dma_heap(" << heap_path
                  << "): " << std::strerror(errno) << std::endl;
        return -1;
    }

    dma_heap_allocation_data req{};
    req.len      = size;
    req.fd_flags = O_RDWR | O_CLOEXEC;

    if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &req) < 0)
    {
        std::cerr << "DMA heap allocation failed: " << std::strerror(errno) << std::endl;
        close(heap);
        return -1;
    }
    close(heap);

    void* mapped_base = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, req.fd, 0);
    if (mapped_base == MAP_FAILED)
    {
        std::cerr << "Failed to mmap dma-buf fd=" << req.fd << ": " << std::strerror(errno)
                  << std::endl;
        close(req.fd);
        return -1;
    }

    output->fd   = req.fd;
    output->base = mapped_base;
    output->size = size;
    return 0;
}

int RgaInstance::EnsureDmaBufFD(MppOutputFD* output, size_t min_size, const char* heap_path)
{
    if (!output || min_size == 0)
    {
        return -1;
    }

    if (output->fd >= 0 && output->size >= min_size)
    {
        return 0;
    }

    return AllocDmaBufFD(output, min_size, heap_path);
}

void RgaInstance::ReleaseDmaBufFD(MppOutputFD* output)
{
    if (!output)
    {
        return;
    }

    if (output->base && output->size > 0)
    {
        (void)munmap(output->base, output->size);
        output->base = nullptr;
    }
    if (output->fd >= 0)
    {
        (void)close(output->fd);
        output->fd = -1;
    }
    output->size = 0;
}

bool RgaInstance::IsConfigValid(const ImageConfig& cfg) const
{
    return cfg.valid && cfg.width > 0 && cfg.height > 0 && cfg.h_stride > 0 && cfg.v_stride > 0;
}

int RgaInstance::SetInputImageConfig(uint32_t width, uint32_t height, uint32_t h_stride,
                                     uint32_t v_stride)
{
    if (width == 0 || height == 0 || h_stride == 0 || v_stride == 0)
    {
        return -1;
    }
    input_cfg_.width    = width;
    input_cfg_.height   = height;
    input_cfg_.h_stride = h_stride;
    input_cfg_.v_stride = v_stride;
    input_cfg_.valid    = true;
    return 0;
}

int RgaInstance::SetOutputImageConfig(uint32_t width, uint32_t height, uint32_t h_stride,
                                      uint32_t v_stride)
{
    if (width == 0 || height == 0 || h_stride == 0 || v_stride == 0)
    {
        return -1;
    }
    output_cfg_.width    = width;
    output_cfg_.height   = height;
    output_cfg_.h_stride = h_stride;
    output_cfg_.v_stride = v_stride;
    output_cfg_.valid    = true;
    return 0;
}

size_t RgaInstance::CalcNv12ImageSize(uint32_t h_stride, uint32_t v_stride) const
{
    return static_cast<size_t>(h_stride) * static_cast<size_t>(v_stride) * 3u / 2u;
}

int RgaInstance::BlitWithConfig(const MppOutputFD* src, const ImageConfig& src_cfg,
                                MppOutputFD* dst, const ImageConfig& dst_cfg,
                                const char* output_heap_path)
{
    if (!initialized_ && RgaInit() != 0)
    {
        return -1;
    }
    if (!src || !dst)
    {
        return -1;
    }
    if (!IsConfigValid(src_cfg) || !IsConfigValid(dst_cfg))
    {
        return -1;
    }

    const size_t src_need = CalcNv12ImageSize(src_cfg.h_stride, src_cfg.v_stride);
    const size_t dst_need = CalcNv12ImageSize(dst_cfg.h_stride, dst_cfg.v_stride);
    if (src->size < src_need)
    {
        std::cerr << "RGA source buffer too small, size=" << src->size << ", need=" << src_need
                  << std::endl;
        return -1;
    }

    if (EnsureDmaBufFD(dst, dst_need, output_heap_path) != 0)
    {
        std::cerr << "Failed to prepare RGA output dma-buf, need=" << dst_need << std::endl;
        return -1;
    }

    rga_buffer_t        src_buffer{};
    rga_buffer_t        dst_buffer{};
    rga_buffer_handle_t src_handle = 0;
    rga_buffer_handle_t dst_handle = 0;

    if (BuildRgaBuffer(src, src_cfg.width, src_cfg.height, src_cfg.h_stride, src_cfg.v_stride,
                       &src_buffer, &src_handle) != 0)
    {
        std::cerr << "Failed to build RGA source buffer, fd=" << src->fd << ", base=" << src->base
                  << ", size=" << src->size << std::endl;
        return -1;
    }

    if (BuildRgaBuffer(dst, dst_cfg.width, dst_cfg.height, dst_cfg.h_stride, dst_cfg.v_stride,
                       &dst_buffer, &dst_handle) != 0)
    {
        if (src_handle)
        {
            (void)releasebuffer_handle(src_handle);
        }
        std::cerr << "Failed to build RGA destination buffer, fd=" << dst->fd
                  << ", base=" << dst->base << ", size=" << dst->size << std::endl;
        return -1;
    }

    IM_STATUS status = IM_STATUS_FAILED;
    if (src_cfg.width == dst_cfg.width && src_cfg.height == dst_cfg.height &&
        src_cfg.h_stride == dst_cfg.h_stride && src_cfg.v_stride == dst_cfg.v_stride)
    {
        status = imcopy(src_buffer, dst_buffer);
    }
    else
    {
        status = imresize(src_buffer, dst_buffer);
    }

    if (src_handle)
    {
        (void)releasebuffer_handle(src_handle);
    }
    if (dst_handle)
    {
        (void)releasebuffer_handle(dst_handle);
    }

    if (status != IM_STATUS_SUCCESS)
    {
        std::cerr << "RGA operation failed: " << imStrError_t(status) << std::endl;
        return -1;
    }

    return 0;
}

int RgaInstance::Blit(const MppOutputFD* src, MppOutputFD* dst, const char* output_heap_path)
{
    if (!IsConfigValid(input_cfg_))
    {
        std::cerr << "RGA input image config is not set" << std::endl;
        return -1;
    }
    const ImageConfig& dst_cfg = IsConfigValid(output_cfg_) ? output_cfg_ : input_cfg_;
    return BlitWithConfig(src, input_cfg_, dst, dst_cfg, output_heap_path);
}

int RgaInstance::Copy(const MppOutputFD* src, MppOutputFD* dst,
                      const char* output_heap_path)
{
    if (!IsConfigValid(input_cfg_))
    {
        std::cerr << "RGA input image config is not set" << std::endl;
        return -1;
    }

    ImageConfig dst_cfg = IsConfigValid(output_cfg_) ? output_cfg_ : input_cfg_;
    return BlitWithConfig(src, input_cfg_, dst, dst_cfg, output_heap_path);
}

int RgaInstance::Resize(const MppOutputFD* src, MppOutputFD* dst,
                        const char* output_heap_path)
{
    if (!IsConfigValid(input_cfg_) || !IsConfigValid(output_cfg_))
    {
        std::cerr << "RGA resize requires both input and output configs" << std::endl;
        return -1;
    }

    return BlitWithConfig(src, input_cfg_, dst, output_cfg_, output_heap_path);
}

int RgaInstance::Rotate(const MppOutputFD* src, MppOutputFD* dst, int angle_deg,
                        const char* output_heap_path)
{
    if (!initialized_ && RgaInit() != 0)
    {
        return -1;
    }
    if (!src || !dst)
    {
        return -1;
    }
    if (!IsConfigValid(input_cfg_))
    {
        std::cerr << "RGA input image config is not set" << std::endl;
        return -1;
    }

    int rotate_mode = 0;
    if (angle_deg == 90)
    {
        rotate_mode = IM_HAL_TRANSFORM_ROT_90;
    }
    else if (angle_deg == 180)
    {
        rotate_mode = IM_HAL_TRANSFORM_ROT_180;
    }
    else if (angle_deg == 270)
    {
        rotate_mode = IM_HAL_TRANSFORM_ROT_270;
    }
    else
    {
        std::cerr << "Unsupported rotate angle: " << angle_deg
                  << " (only 90/180/270 supported)" << std::endl;
        return -1;
    }

    ImageConfig dst_cfg = IsConfigValid(output_cfg_) ? output_cfg_ : input_cfg_;
    if (angle_deg == 90 || angle_deg == 270)
    {
        const uint32_t expected_width    = input_cfg_.height;
        const uint32_t expected_height   = input_cfg_.width;
        const uint32_t expected_h_stride = Align16(expected_width);
        const uint32_t expected_v_stride = Align16(expected_height);

        const bool size_mismatch =
            (dst_cfg.width != expected_width || dst_cfg.height != expected_height);
        const bool stride_mismatch =
            (dst_cfg.h_stride < expected_width || dst_cfg.v_stride < expected_height);
        if (size_mismatch || stride_mismatch)
        {
            std::cerr << "Warning: rotate " << angle_deg << " output config mismatch, configured "
                      << dst_cfg.width << "x" << dst_cfg.height << " stride(" << dst_cfg.h_stride
                      << "," << dst_cfg.v_stride << "), auto-adjust to " << expected_width << "x"
                      << expected_height << " stride(" << expected_h_stride << ","
                      << expected_v_stride << ")" << std::endl;
            dst_cfg.width    = expected_width;
            dst_cfg.height   = expected_height;
            dst_cfg.h_stride = expected_h_stride;
            dst_cfg.v_stride = expected_v_stride;
            dst_cfg.valid    = true;
        }
    }

    const size_t src_need = CalcNv12ImageSize(input_cfg_.h_stride, input_cfg_.v_stride);
    const size_t dst_need = CalcNv12ImageSize(dst_cfg.h_stride, dst_cfg.v_stride);
    if (src->size < src_need)
    {
        std::cerr << "RGA source buffer too small, size=" << src->size << ", need=" << src_need
                  << std::endl;
        return -1;
    }
    if (EnsureDmaBufFD(dst, dst_need, output_heap_path) != 0)
    {
        std::cerr << "Failed to prepare RGA rotate output dma-buf, need=" << dst_need
                  << std::endl;
        return -1;
    }

    rga_buffer_t        src_buffer{};
    rga_buffer_t        dst_buffer{};
    rga_buffer_handle_t src_handle = 0;
    rga_buffer_handle_t dst_handle = 0;
    if (BuildRgaBuffer(src, input_cfg_.width, input_cfg_.height, input_cfg_.h_stride,
                       input_cfg_.v_stride, &src_buffer, &src_handle) != 0)
    {
        std::cerr << "Failed to build RGA rotate source buffer" << std::endl;
        return -1;
    }
    if (BuildRgaBuffer(dst, dst_cfg.width, dst_cfg.height, dst_cfg.h_stride, dst_cfg.v_stride,
                       &dst_buffer, &dst_handle) != 0)
    {
        ReleaseRgaHandle(src_handle);
        std::cerr << "Failed to build RGA rotate destination buffer" << std::endl;
        return -1;
    }

    IM_STATUS status = imrotate(src_buffer, dst_buffer, rotate_mode);
    ReleaseRgaHandle(src_handle);
    ReleaseRgaHandle(dst_handle);

    if (status != IM_STATUS_SUCCESS)
    {
        std::cerr << "RGA rotate failed: " << imStrError_t(status) << std::endl;
        return -1;
    }
    return 0;
}

int RgaInstance::Crop(const MppOutputFD* src, MppOutputFD* dst, uint32_t x, uint32_t y,
                      uint32_t crop_width, uint32_t crop_height, const char* output_heap_path)
{
    if (!initialized_ && RgaInit() != 0)
    {
        return -1;
    }
    if (!src || !dst)
    {
        return -1;
    }
    if (!IsConfigValid(input_cfg_))
    {
        std::cerr << "RGA input image config is not set" << std::endl;
        return -1;
    }
    if (crop_width == 0 || crop_height == 0)
    {
        return -1;
    }

    // NV12 裁剪要求偶数对齐，避免 UV 面采样错位。
    if ((x & 1u) || (y & 1u) || (crop_width & 1u) || (crop_height & 1u))
    {
        std::cerr << "NV12 crop requires even x/y/width/height, got x=" << x << ", y=" << y
                  << ", w=" << crop_width << ", h=" << crop_height << std::endl;
        return -1;
    }
    if (x + crop_width > input_cfg_.width || y + crop_height > input_cfg_.height)
    {
        std::cerr << "Crop rect out of source bounds" << std::endl;
        return -1;
    }

    ImageConfig dst_cfg{};
    dst_cfg.width    = crop_width;
    dst_cfg.height   = crop_height;
    dst_cfg.h_stride = Align16(crop_width);
    dst_cfg.v_stride = Align16(crop_height);
    dst_cfg.valid    = true;

    const size_t src_need = CalcNv12ImageSize(input_cfg_.h_stride, input_cfg_.v_stride);
    const size_t dst_need = CalcNv12ImageSize(dst_cfg.h_stride, dst_cfg.v_stride);
    if (src->size < src_need)
    {
        std::cerr << "RGA source buffer too small, size=" << src->size << ", need=" << src_need
                  << std::endl;
        return -1;
    }
    if (EnsureDmaBufFD(dst, dst_need, output_heap_path) != 0)
    {
        std::cerr << "Failed to prepare RGA crop output dma-buf, need=" << dst_need << std::endl;
        return -1;
    }

    rga_buffer_t        src_buffer{};
    rga_buffer_t        dst_buffer{};
    rga_buffer_handle_t src_handle = 0;
    rga_buffer_handle_t dst_handle = 0;
    if (BuildRgaBuffer(src, input_cfg_.width, input_cfg_.height, input_cfg_.h_stride,
                       input_cfg_.v_stride, &src_buffer, &src_handle) != 0)
    {
        std::cerr << "Failed to build RGA crop source buffer" << std::endl;
        return -1;
    }
    if (BuildRgaBuffer(dst, dst_cfg.width, dst_cfg.height, dst_cfg.h_stride, dst_cfg.v_stride,
                       &dst_buffer, &dst_handle) != 0)
    {
        ReleaseRgaHandle(src_handle);
        std::cerr << "Failed to build RGA crop destination buffer" << std::endl;
        return -1;
    }

    im_rect rect{};
    rect.x      = static_cast<int>(x);
    rect.y      = static_cast<int>(y);
    rect.width  = static_cast<int>(crop_width);
    rect.height = static_cast<int>(crop_height);
    IM_STATUS status = imcrop(src_buffer, dst_buffer, rect);

    ReleaseRgaHandle(src_handle);
    ReleaseRgaHandle(dst_handle);
    if (status != IM_STATUS_SUCCESS)
    {
        std::cerr << "RGA crop failed: " << imStrError_t(status) << std::endl;
        return -1;
    }
    return 0;
}
