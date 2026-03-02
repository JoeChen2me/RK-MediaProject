# NPU 集成实施记录（RK3588 + aarch64）

## 1. 实施结论

本轮已完成 `NV12 -> RKNN 推理 -> RGA 画框 -> MPP 编码 -> ZLM 推流` 的代码接入，且按功能拆分为 5 个 commit 执行。

默认构建配置固定为：
- `TARGET_SOC=rk3588`
- `TARGET_ARCH=aarch64`
- `CMAKE_BUILD_TYPE=Debug`
- `USE_RGA=ON`
- `USE_LIBJPEG=ON`

同时以上项均支持通过 CMake 参数覆盖。

## 2. 代码落地范围

### 2.1 Commit 列表（实际）

1. `fe1ae2d`  
   `build(cmake): 引入rknn_model_zoo子cmake并固化rk3588默认配置`
2. `28be366`  
   `feat(npu-api): 定义RknnYoloInfer接口与检测结果类型`
3. `cee2f3e`  
   `feat(npu-core): 实现yolov8 rknn推理与nv12画框`
4. `4539316`  
   `feat(pipeline): 新增npu线程接入解码到编码主链路`
5. `（当前文档提交）`  
   `chore(doc): 补充npu集成验收与cmake参数说明`

### 2.2 关键文件

- 主构建：`/home/joe/Code/V4L2-Develop/Basic_WthoutAI/CMakeLists.txt`
- NPU 接口：`/home/joe/Code/V4L2-Develop/Basic_WthoutAI/include/rknnInfer.h`
- NPU 实现：`/home/joe/Code/V4L2-Develop/Basic_WthoutAI/src/rknnyolo.cpp`
- 主链路接入：`/home/joe/Code/V4L2-Develop/Basic_WthoutAI/src/main.cpp`

## 3. 运行架构（已实现）

### 3.1 线程与队列

- `capture_thread`：采集摄像头帧，放入 `readyQueue`
- `decode_thread`：MPP 解码，输出放入 `rgaConsumeQueue`
- `rga_thread`：RGA 处理后输出 NV12 到 `npuInputQueue`
- `npu_thread`：调用 `RknnYoloInfer::InferAndDraw`，结果帧放入 `mppEncodeInputQueue`
- `mpp_encode_input_thread`：送 `encode_put_frame`
- `mpp_encode_output_thread`：取编码包并 `InputPacketChunk` 到 ZLM

### 3.2 拥塞与降级策略

- `npuInputQueue` 达上限时：丢最旧帧并回收，避免端到端延迟无限累积
- NPU 推理失败：无框直通编码，不中断编码/推流
- 编码推送临时失败：有限重试 + 退避，超过阈值再触发停机

## 4. CMake 参数与使用方式

### 4.1 默认行为

- 默认 `Debug`
- 默认启用 `RGA`、`libjpeg`
- 默认目标平台 `rk3588 + aarch64`

### 4.2 可覆盖参数

```bash
-DTARGET_SOC=<rk3588|rk3576|rk356x|rv1106|rv1103|rv1126b|rk1808|rv1109|rv1126>
-DTARGET_ARCH=<aarch64|armhf>
-DCMAKE_BUILD_TYPE=<Debug|Release|RelWithDebInfo|MinSizeRel>
-DUSE_RGA=<ON|OFF>
-DUSE_LIBJPEG=<ON|OFF>
```

说明：项目内部会把 `USE_RGA/USE_LIBJPEG` 映射到 model_zoo 子 CMake 的 `DISABLE_RGA/DISABLE_LIBJPEG`，无需手工改子工程。

## 5. 构建与运行命令

### 5.1 默认构建

```bash
cd /home/joe/Code/V4L2-Develop/Basic_WthoutAI
cmake -S . -B build
cmake --build build -j4
```

### 5.2 覆盖配置构建

```bash
cd /home/joe/Code/V4L2-Develop/Basic_WthoutAI
cmake -S . -B build_rel -DCMAKE_BUILD_TYPE=Release -DUSE_RGA=OFF -DUSE_LIBJPEG=OFF
cmake --build build_rel -j4
```

### 5.3 运行

```bash
cd /home/joe/Code/V4L2-Develop/Basic_WthoutAI/build/bin
./app
```

模型默认路径：
`/home/joe/Code/V4L2-Develop/Basic_WthoutAI/experiments/modelOnnx/yolov8n_rk3588_i8.rknn`

## 6. 验收清单

### 6.1 已完成验证

- `cmake -S . -B build` 通过
- `cmake --build build -j4` 通过
- `cmake -S . -B build_rel -DCMAKE_BUILD_TYPE=Release -DUSE_RGA=OFF -DUSE_LIBJPEG=OFF` 配置通过

### 6.2 运行期观测点（代码已补齐）

NPU 周期日志包含：
- `processed` / `fail` / `drop`
- `infer_avg_ms` / `infer_max_ms`
- `npu_q=当前深度/峰值`
- `enc_q=当前深度/峰值`

可用于判断：
- NPU 是否成为瓶颈
- 队列是否持续堆积
- 丢帧策略是否按预期触发

### 6.3 建议现场验收步骤

1. 启动程序并确认 RTSP/WebRTC 持续出流。
2. 观察目标框是否稳定叠加在画面上。
3. 人工提高负载，确认 `drop` 计数上升但推流不断流。
4. 临时制造模型加载/推理失败，确认无框直通仍可编码推流。

## 7. 设计边界说明

- 未复制 `experiments/rknn_model_zoo/3rdparty`，仅通过 `add_subdirectory` 引用其导出变量。
- 未改动 `IO_FD_t`、`MppEncInstance`、`RgaInstance` 对外签名。
- NPU 推理阶段独立线程接入，尽量减少对原有编码与 ZLM 输出契约的影响。
