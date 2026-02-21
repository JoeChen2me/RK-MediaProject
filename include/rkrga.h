#ifndef RK_RGA_H
#define RK_RGA_H

#include "pubDataType.h"

#include <cstddef>
#include <cstdint>

class RgaInstance
{
   public:
    RgaInstance();
    ~RgaInstance();

    int RgaInit();

    // 申请并映射一个输出 dma-buf，结果写入 output。
    int AllocDmaBufFD(MppOutputFD* output, size_t size,
                      const char* heap_path = "/dev/dma_heap/system");
    // 确保 output 至少有 min_size 大小的 dma-buf；不足时重新申请。
    int EnsureDmaBufFD(MppOutputFD* output, size_t min_size,
                       const char* heap_path = "/dev/dma_heap/system");
    // 释放 output 对应的 mmap 和 fd。
    void ReleaseDmaBufFD(MppOutputFD* output);

    // 固定分辨率场景下推荐在初始化阶段设置一次输入/输出图像参数。
    int SetInputImageConfig(uint32_t width, uint32_t height, uint32_t h_stride,
                            uint32_t v_stride);
    int SetOutputImageConfig(uint32_t width, uint32_t height, uint32_t h_stride,
                             uint32_t v_stride);

    // 使用类内配置执行 blit：配置一致走 copy，不一致走 resize（固定 NV12，不做 csc）。
    int Blit(const MppOutputFD* src, MppOutputFD* dst,
             const char* output_heap_path = "/dev/dma_heap/system");
    // 便捷接口：语义上用于同格式拷贝；若输入输出配置不同会退化为 resize。
    int Copy(const MppOutputFD* src, MppOutputFD* dst,
             const char* output_heap_path = "/dev/dma_heap/system");
    // 缩放到 output 配置尺寸。
    int Resize(const MppOutputFD* src, MppOutputFD* dst,
               const char* output_heap_path = "/dev/dma_heap/system");
    // 旋转（仅支持 90/180/270 度）。
    int Rotate(const MppOutputFD* src, MppOutputFD* dst, int angle_deg,
               const char* output_heap_path = "/dev/dma_heap/system");
    // 裁剪：截取输入图像中 [x, y, crop_width, crop_height] 区域，输出为该区域大小，不做缩放。
    int Crop(const MppOutputFD* src, MppOutputFD* dst, uint32_t x, uint32_t y,
             uint32_t crop_width, uint32_t crop_height,
             const char* output_heap_path = "/dev/dma_heap/system");

   private:
    struct ImageConfig
    {
        uint32_t width    = 0;
        uint32_t height   = 0;
        uint32_t h_stride = 0;
        uint32_t v_stride = 0;
        bool     valid    = false;
    };

    bool   IsConfigValid(const ImageConfig& cfg) const;
    size_t CalcNv12ImageSize(uint32_t h_stride, uint32_t v_stride) const;
    int    BlitWithConfig(const MppOutputFD* src, const ImageConfig& src_cfg, MppOutputFD* dst,
                          const ImageConfig& dst_cfg, const char* output_heap_path);

    ImageConfig input_cfg_{};
    ImageConfig output_cfg_{};
    bool initialized_ = false;
};

#endif  // RK_RGA_H
