#ifndef RK_RGA_H
#define RK_RGA_H

#include "pubDataType.h"

#include <cstddef>
#include <cstdint>

namespace
{
constexpr const char* kRgaDmaHeapPath = "/dev/dma_heap/system";
}

class RgaInstance
{
   public:
    RgaInstance();
    ~RgaInstance();

    int RgaInit();
    const IO_FD_t* Copy(const IO_FD_t* src);
    const IO_FD_t* FlipHorizontal(const IO_FD_t* src);
    const IO_FD_t* FlipVertical(const IO_FD_t* src);

   private:
    struct ImageConfig
    {
        uint32_t width    = 0;
        uint32_t height   = 0;
        uint32_t h_stride = 0;
        uint32_t v_stride = 0;
        bool     valid    = false;
    };

    enum class Operation
    {
        kCopy,
        kFlipHorizontal,
        kFlipVertical,
    };

    int            AllocDmaBufFD(IO_FD_t* output, size_t size);
    void           ReleaseDmaBufFD(IO_FD_t* output);
    bool           IsConfigValid(const ImageConfig& cfg) const;
    bool           LoadConfigFromIO(const IO_FD_t* io, ImageConfig* cfg) const;
    int            InitOutputPoolIfNeeded(const ImageConfig& src_cfg);
    void           ReleaseOutputPool();
    IO_FD_t*       AcquireOutputPoolBuffer();
    const IO_FD_t* TransformInternal(const IO_FD_t* src, Operation op);
    size_t         CalcNv12ImageSize(uint32_t h_stride, uint32_t v_stride) const;
    bool                    initialized_                   = false;
    IO_FD_t                 output_pool_[resource_limits::kRgaOutputBufferCount] = {};
    bool                    output_pool_ready_             = false;
    size_t                  output_pool_index_             = 0;
};

#endif  // RK_RGA_H
