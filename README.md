# Rockchip  零拷贝完整视频链路

基于 Rockchip 平台的端到端实时视频处理示例工程，核心链路为：

`V4L2(USB MJPEG 相机) -> MPP 解码 -> RGA 处理 -> MPP H.264 编码 -> ZLMediaKit 推流`

项目当前目标是提供一个可运行、可扩展、便于调试的纯 C++ 多线程视频流水线基础框架。

## 功能概览

- 使用 V4L2 采集 USB 摄像头画面（`/dev/video0`，默认 MJPEG 1080p@30fps）
- 使用 Rockchip MPP 解码 MJPEG（输入零拷贝，DMABUF）
- 使用 Rockchip RGA 做图像搬运/格式统一（输出 NV12）
- 使用 Rockchip MPP 编码 H.264（默认 CBR 2Mbps，GOP=60）
- 内置 ZLMediaKit C API 发布 RTSP，并提供 WebRTC 播放协商 API
- 全流程多线程解耦，支持优雅退出与资源回收

## 流水线架构

```text
Capture Thread
  V4L2 DQBUF (DMABUF, MJPEG)
      -> readyQueue

Decode Thread
  MPP Decode (MJPEG -> NV12)
      -> rgaConsumeQueue
      -> recycleQueue(回收相机buffer)

RGA Thread
  RGA Copy/Convert (保持 NV12 输出)
      -> mppEncodeInputQueue
      -> mppRecycleOutputQueue(回收解码输出)

Encode Input Thread
  MPP encode_put_frame
      -> 回收 RGA 输出池

Encode Output Thread
  MPP encode_get_packet (H264 Annex-B)
      -> ZLM input (RTSP / WebRTC)
```

## 目录说明

- `src/`
  - `main.cpp`：主流程与线程调度
  - `v4l2Camera.cpp`：V4L2 采集与 DMABUF 池管理
  - `rkmppdec.cpp`：MPP MJPEG 解码（外部缓冲模式）
  - `rkrga.cpp`：RGA 转换/拷贝与输出池
  - `rkmppenc.cpp`：MPP H.264 编码
  - `zlm_publisher.cpp`：ZLMediaKit 发布与 WebRTC API 适配
- `include/`：项目头文件
- `include/ZLM/`：ZLMediaKit C API 头文件
- `lib/`：本地动态库（`libmk_api.so`、`libZLToolKit.so`）
- `experiments/`：实验目录（MPP 源码、RKNN 相关、模型等）
- `Plan/`：设计与接入方案记录
- `webrtc_play_test.html`：浏览器端 WebRTC 播放测试页面

## 环境依赖

建议运行环境：Rockchip Linux（如 RK3588 / OrangePi 5 Plus）。

构建与运行需满足：

- CMake >= 3.16
- C++17 编译器（g++/clang++）
- 系统安装 V4L2、Rockchip MPP、RGA 开发库
  - 头文件路径：`/usr/include/rockchip`、`/usr/include/rga`
  - 动态库：`rockchip_mpp`、`rga`、`v4l2`
- 本仓库 `lib/` 下提供 ZLMediaKit C API 动态库

## 编译

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
```

编译产物默认在：

- `build/bin/app`

## 运行

```bash
./build/bin/app
```

程序启动后会在日志中打印：

- RTSP 地址：`rtsp://<设备IP>:8554/live/camera`
- WebRTC 协商 API：`http://<设备IP>:8000/index/api/webrtc?app=live&stream=camera&type=play`

按 `Ctrl+C` 触发优雅退出。

## 拉流验证

RTSP：

```bash
ffplay rtsp://<设备IP>:8554/live/camera
```

WebRTC：

1. 打开仓库根目录的 `webrtc_play_test.html`
2. 将页面中的 API 地址改为实际设备 IP
3. 点击“开始播放”

## 当前默认配置（代码内硬编码）

- 相机设备：`/dev/video0`（`src/main.cpp`）
- 采集参数：`1920x1080 @ 30fps, MJPEG`（`include/v4l2Camera.h`）
- 编码参数：`H.264, CBR 2Mbps, GOP=60`（`include/rkmppenc.h`）
- 发布参数：`app=live, stream=camera, RTSP:8554, HTTP:8000, RTC:8001`（`include/zlm_publisher.h`）

如果需要改配置，建议先改上述头文件常量或主流程初始化参数。

## 已知限制

- 当前仅视频链路（无音频）
- 输入相机需支持 MJPEG（初始化阶段会做能力检查）
- 主要面向 Rockchip 平台（依赖 MPP/RGA）
- 当前暂无命令行参数，配置以源码常量为主

## 参考资料

- 本仓库 `AGENTS.md` 中已整理 Rockchip MPP/RGA 与 OrangePi 文档路径
- `experiments/mpp_src_try/mpp/` 下保留了官方 MPP 源码与示例，便于对照实现

