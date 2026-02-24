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

namespace
{
constexpr int kRgaFormatNv12 = RK_FORMAT_YCbCr_420_SP;

// 注意：这里的 h_stride / v_stride 是沿用本模块历史命名：
// h_stride -> librga 的 wstride（水平步幅，像素）
// v_stride -> librga 的 hstride（垂直步幅，像素）
int BuildRgaBuffer(const IO_FD_t* buffer, uint32_t width, uint32_t height, uint32_t h_stride,
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

RgaInstance::RgaInstance()
{
    for (auto& item : output_pool_)
    {
        item.fd         = -1;
        item.base       = nullptr;
        item.size       = 0;
        item.width      = 0;
        item.height     = 0;
        item.hor_stride = 0;
        item.ver_stride = 0;
    }
}

RgaInstance::~RgaInstance() { ReleaseOutputPool(); }

int RgaInstance::RgaInit()
{
    IM_STATUS ret = imcheckHeader();
    if (ret != IM_STATUS_SUCCESS)
    {
        std::cerr << "RGA header/runtime check failed: " << imStrError_t(ret) << std::endl;
        return -1;
    }
    std::cout << querystring(RGA_ALL) << std::endl;  // 输出 RGA 完整信息
    initialized_ = true;
    return 0;
}

int RgaInstance::AllocDmaBufFD(IO_FD_t* output, size_t size)
{
    if (!output || size == 0)
    {
        return -1;
    }

    ReleaseDmaBufFD(output);

    int heap = open(kRgaDmaHeapPath, O_RDWR | O_CLOEXEC);
    if (heap < 0)
    {
        std::cerr << "Failed to open dma_heap(" << kRgaDmaHeapPath << "): " << std::strerror(errno)
                  << std::endl;
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

    output->fd         = req.fd;
    output->base       = mapped_base;
    output->size       = size;
    output->width      = 0;
    output->height     = 0;
    output->hor_stride = 0;
    output->ver_stride = 0;
    return 0;
}

void RgaInstance::ReleaseDmaBufFD(IO_FD_t* output)
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
    output->size       = 0;
    output->width      = 0;
    output->height     = 0;
    output->hor_stride = 0;
    output->ver_stride = 0;
}

bool RgaInstance::IsConfigValid(const ImageConfig& cfg) const
{
    return cfg.valid && cfg.width > 0 && cfg.height > 0 && cfg.h_stride > 0 && cfg.v_stride > 0;
}

bool RgaInstance::LoadConfigFromIO(const IO_FD_t* io, ImageConfig* cfg) const
{
    if (!io || !cfg)
    {
        return false;
    }
    if (io->width == 0 || io->height == 0 || io->hor_stride == 0 || io->ver_stride == 0)
    {
        return false;
    }

    cfg->width  = io->width;
    cfg->height = io->height;
    // 历史命名映射：h_stride 保存水平步幅（wstride）。
    cfg->h_stride = io->hor_stride;
    // 历史命名映射：v_stride 保存垂直步幅（hstride）。
    cfg->v_stride = io->ver_stride;
    cfg->valid    = true;
    return true;
}

size_t RgaInstance::CalcNv12ImageSize(uint32_t h_stride, uint32_t v_stride) const
{
    return static_cast<size_t>(h_stride) * static_cast<size_t>(v_stride) * 3u / 2u;
}

// 功能：重置输出池队列状态。
// 行为：先清空“可用队列”和“待回收队列”；若输出池已就绪，则把池内所有描述符重新入可用队列。
void RgaInstance::ResetOutputPoolQueueState()
{
    // std::queue 无 clear() 接口，使用与空队列 swap 的方式清空历史元素。
    std::queue<IO_FD_t*> empty_available_queue;
    std::queue<IO_FD_t*> empty_pending_recycle_queue;
    output_pool_available_queue_.swap(empty_available_queue);
    output_pool_pending_recycle_queue_.swap(empty_pending_recycle_queue);

    if (!output_pool_ready_)
    {
        return;
    }

    // 池就绪后，把固定资源池里的每个 IO_FD_t* 重新放回可用队列。
    for (size_t i = 0; i < resource_limits::kRgaOutputBufferCount; ++i)
    {
        output_pool_available_queue_.push(&output_pool_[i]);
    }
}

// 功能：把待回收队列中的描述符转移到可用队列。
// 约束：仅做描述符搬运，不涉及内存释放。
void RgaInstance::DrainPendingRecycleQueue()
{
    // 将待回收队列中的描述符回灌到可用队列，供下一次 Acquire 复用。
    while (!output_pool_pending_recycle_queue_.empty())
    {
        IO_FD_t* out = output_pool_pending_recycle_queue_.front();
        output_pool_pending_recycle_queue_.pop();
        if (!out)
        {
            continue;
        }
        output_pool_available_queue_.push(out);
    }
}

// 功能：判断给定描述符是否属于当前 RGA 实例维护的输出池。
bool RgaInstance::IsOutputPoolMember(const IO_FD_t* output_desc) const
{
    if (!output_desc)
    {
        return false;
    }

    const IO_FD_t* begin = &output_pool_[0];
    const IO_FD_t* end   = begin + resource_limits::kRgaOutputBufferCount;
    return output_desc >= begin && output_desc < end;
}

// 功能：按源图像配置初始化输出池；若池已存在则做参数一致性校验。
int RgaInstance::InitOutputPoolIfNeeded(const ImageConfig& src_cfg)
{
    if (!IsConfigValid(src_cfg))
    {
        std::cerr << "Failed to initialize output pool: source config is invalid" << std::endl;
        return -1;
    }

    const size_t SizeNeed = CalcNv12ImageSize(src_cfg.h_stride, src_cfg.v_stride);
    if (output_pool_ready_)
    {
        const IO_FD_t& ref = output_pool_[0];
        if (ref.width != src_cfg.width || ref.height != src_cfg.height ||
            ref.hor_stride != src_cfg.h_stride || ref.ver_stride != src_cfg.v_stride ||
            ref.size < SizeNeed)
        {
            std::cerr << "RGA output pool geometry mismatch with source, expect " << src_cfg.width
                      << "x" << src_cfg.height << " stride(" << src_cfg.h_stride << ","
                      << src_cfg.v_stride << "), pool has " << ref.width << "x" << ref.height
                      << " stride(" << ref.hor_stride << "," << ref.ver_stride << ")"
                      << ", size=" << ref.size << ", SizeNeed=" << SizeNeed << std::endl;
            return -1;
        }
        return 0;
    }

    for (size_t i = 0; i < resource_limits::kRgaOutputBufferCount; ++i)
    {
        IO_FD_t& out = output_pool_[i];
        if (AllocDmaBufFD(&out, SizeNeed) != 0)
        {
            std::cerr << "Failed to allocate output pool buffer, index=" << i << std::endl;
            ReleaseOutputPool();
            return -1;
        }
        out.width      = src_cfg.width;
        out.height     = src_cfg.height;
        out.hor_stride = src_cfg.h_stride;
        out.ver_stride = src_cfg.v_stride;
    }

    output_pool_ready_ = true;
    ResetOutputPoolQueueState();
    return 0;
}

// 功能：释放输出池的 dma-buf 资源，并同步重置队列状态。
void RgaInstance::ReleaseOutputPool()
{
    for (auto& out : output_pool_)
    {
        ReleaseDmaBufFD(&out);
    }
    output_pool_ready_ = false;
    ResetOutputPoolQueueState();
}

// 功能：从可用队列获取一个可写输出描述符。
// 行为：获取前先处理待回收队列，使最近归还的资源能够被立即复用。
IO_FD_t* RgaInstance::AcquireOutputPoolBuffer()
{
    if (!output_pool_ready_)
    {
        return nullptr;
    }

    DrainPendingRecycleQueue();
    if (output_pool_available_queue_.empty())
    {
        return nullptr;
    }

    IO_FD_t* out = output_pool_available_queue_.front();
    output_pool_available_queue_.pop();
    return out;
}

// 功能：将已消费的输出描述符加入待回收队列。
// 场景：当前主要用于错误路径兜底；后续可由外部消费者在消费完成后调用。
// 说明：待回收队列在下次 Acquire 时统一回灌到可用队列。
int RgaInstance::RgaQueueOutputForRecycle(const IO_FD_t* output_desc)
{
    if (!output_desc || !output_pool_ready_ || !IsOutputPoolMember(output_desc))
    {
        std::cerr << "Invalid RGA output descriptor for recycle, fd="
                  << ((output_desc) ? output_desc->fd : -1) << std::endl;
        return -1;
    }

    output_pool_pending_recycle_queue_.push(const_cast<IO_FD_t*>(output_desc));
    return 0;
}

const IO_FD_t* RgaInstance::TransformInternal(const IO_FD_t* src, Operation op)
{
    if (!initialized_)
    {
        std::cerr << "RGA is not initialized, call RgaInit first" << std::endl;
        return nullptr;
    }
    if (!src)
    {
        return nullptr;
    }

    ImageConfig src_cfg{};
    if (!LoadConfigFromIO(src, &src_cfg))
    {
        std::cerr << "RGA source config is invalid in source IO_FD_t metadata" << std::endl;
        return nullptr;
    }

    if (InitOutputPoolIfNeeded(src_cfg) != 0)
    {
        return nullptr;
    }

    const size_t src_need = CalcNv12ImageSize(src_cfg.h_stride, src_cfg.v_stride);
    if (src->size < src_need)
    {
        std::cerr << "RGA source buffer too small, size=" << src->size << ", SizeNeed=" << src_need
                  << std::endl;
        return nullptr;
    }

    IO_FD_t* dst = AcquireOutputPoolBuffer();
    if (!dst)
    {
        std::cerr << "RGA output pool has no free buffer to acquire" << std::endl;
        return nullptr;
    }

    rga_buffer_t        src_buffer{};
    rga_buffer_t        dst_buffer{};
    rga_buffer_handle_t src_handle            = 0;
    rga_buffer_handle_t dst_handle            = 0;
    auto                recycle_dst_if_needed = [&]() { (void)RgaQueueOutputForRecycle(dst); };

    if (BuildRgaBuffer(src, src_cfg.width, src_cfg.height, src_cfg.h_stride, src_cfg.v_stride,
                       &src_buffer, &src_handle) != 0)
    {
        recycle_dst_if_needed();
        std::cerr << "Failed to build RGA source buffer" << std::endl;
        return nullptr;
    }

    if (BuildRgaBuffer(dst, src_cfg.width, src_cfg.height, src_cfg.h_stride, src_cfg.v_stride,
                       &dst_buffer, &dst_handle) != 0)
    {
        ReleaseRgaHandle(src_handle);
        recycle_dst_if_needed();
        std::cerr << "Failed to build RGA destination buffer" << std::endl;
        return nullptr;
    }

    IM_STATUS status = IM_STATUS_FAILED;
    switch (op)
    {
        case Operation::kCopy:
            status = imcopy(src_buffer, dst_buffer);
            break;
        case Operation::kFlipHorizontal:
            status = imflip(src_buffer, dst_buffer, IM_HAL_TRANSFORM_FLIP_H);
            break;
        case Operation::kFlipVertical:
            status = imflip(src_buffer, dst_buffer, IM_HAL_TRANSFORM_FLIP_V);
            break;
        default:
            status = IM_STATUS_FAILED;
            break;
    }

    ReleaseRgaHandle(src_handle);
    ReleaseRgaHandle(dst_handle);

    if (status != IM_STATUS_SUCCESS)
    {
        recycle_dst_if_needed();
        std::cerr << "RGA operation failed: " << imStrError_t(status) << std::endl;
        return nullptr;
    }

    return dst;
}

const IO_FD_t* RgaInstance::Copy(const IO_FD_t* src)
{
    return TransformInternal(src, Operation::kCopy);
}

const IO_FD_t* RgaInstance::FlipHorizontal(const IO_FD_t* src)
{
    return TransformInternal(src, Operation::kFlipHorizontal);
}

const IO_FD_t* RgaInstance::FlipVertical(const IO_FD_t* src)
{
    return TransformInternal(src, Operation::kFlipVertical);
}
