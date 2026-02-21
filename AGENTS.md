# Basic_WthoutAI 文档记录

> 本文件用于记录本项目依赖的外部手册文档位置，便于在香橙派本机快速查阅。

## 文档存放目录

- `/home/joe/Documents/DevelopDoc`

## 文档手册

1. `mpp 开发手册.md`
   - 路径：`/home/joe/Documents/DevelopDoc/mpp 开发手册.md`
   - 用途：MPP 编解码接口、流程与参数参考。

2. `OrangePi5Plus-用户手册-md 格式.md`
   - 路径：`/home/joe/Documents/DevelopDoc/OrangePi5Plus-用户手册-md 格式.md`
   - 用途：板级硬件、系统配置与外设使用参考。

3. `Rockchip_Developer_Guide_RGA_CN.md`
   - 路径：`/home/joe/Documents/DevelopDoc/Rockchip_Developer_Guide_RGA_CN.md`
   - 用途：RGA 图像处理接口、缩放/色彩转换与性能优化参考。

## Rockchip 官方示例代码位置

> 说明：以下路径为本机已拉取的 Rockchip MPP 源码示例，用于对照手册验证接口调用顺序与内存模式实现。

- MPP 源码根目录：
  - `/home/joe/Code/V4L2-Develop/Basic_WthoutAI/experiments/mpp_src_try/mpp`

- 解码测试示例（重点）：
  - `/home/joe/Code/V4L2-Develop/Basic_WthoutAI/experiments/mpp_src_try/mpp/test/mpi_dec_test.c`
  - `/home/joe/Code/V4L2-Develop/Basic_WthoutAI/experiments/mpp_src_try/mpp/test/mpi_dec_multi_test.c`

- 解码缓冲管理实现（模式1/2/3 关键实现）：
  - `/home/joe/Code/V4L2-Develop/Basic_WthoutAI/experiments/mpp_src_try/mpp/utils/mpi_dec_utils.c`
  - `/home/joe/Code/V4L2-Develop/Basic_WthoutAI/experiments/mpp_src_try/mpp/utils/mpi_dec_utils.h`

- 控制命令定义（control cmd 对照）：
  - `/home/joe/Code/V4L2-Develop/Basic_WthoutAI/experiments/mpp_src_try/mpp/inc/rk_mpi_cmd.h`
