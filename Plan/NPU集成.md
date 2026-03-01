# NPU 集成方案（MPP 解码 NV12 -> RKNN 推理 -> OSD 画框 -> MPP 编码 H264 -> RTSP/WebRTC）

## 1. 目标与范围

### 1.1 目标
在现有链路基础上实现以下闭环：
1. MPP 解码输出 NV12（`MppFrame` 对应 DMABUF）
2. 将帧送入 RK NPU 进行目标检测推理（YOLOv8）
3. 在图像上绘制检测框
4. 送入 MPP 编码器编码为 H264
5. 复用现有 ZLM 通道推送 RTSP / WebRTC

### 1.2 范围内
- C++ 接入 RKNN runtime（`librknnrt`）
- 与现有多线程流水线集成
- NV12 图像的推理前处理与 OSD
- 性能与稳定性验证（吞吐、延迟、丢帧、资源占用）

### 1.3 范围外
- 模型训练与重新量化
- Web 前端播放器改造
- 多模型调度

## 2. 已验证事实（基于本机与仓库）

### 2.1 当前工程主链路可用
当前 `src/main.cpp` 已实现：
`V4L2(MJPEG) -> MPP 解码 -> RGA -> MPP 编码(H264) -> ZLM 推流`

关键代码位置：
- 主流程与线程队列：`/home/joe/Code/V4L2-Develop/Basic_WthoutAI/src/main.cpp`
- 解码模式3外部缓冲：`/home/joe/Code/V4L2-Develop/Basic_WthoutAI/src/rkmppdec.cpp`
- RGA 输出 NV12 池：`/home/joe/Code/V4L2-Develop/Basic_WthoutAI/src/rkrga.cpp`
- 编码输入 NV12 约束：`/home/joe/Code/V4L2-Develop/Basic_WthoutAI/src/rkmppenc.cpp`

### 2.2 设备与运行时可用性
本机（Orange Pi 5 Plus / RK3588 / Ubuntu 22.04.5 aarch64）验证结果：
- `librknnrt.so` 已存在：`/lib/librknnrt.so`
- NPU 节点存在：`/dev/dri/renderD129`，`ID_PATH=platform-fdab0000.npu`
- 当前用户 `joe` 已具备该节点 ACL 读写权限
- `aarch64-linux-gnu-gcc/g++` 可用

结论：具备本机直接编译和本机 NPU 调用的基础条件。

### 2.3 编译可行性实测
已实测执行：
- `experiments/rknn_model_zoo/build-linux.sh -t rk3588 -a aarch64 -d yolov8`

结果：
- 配置/编译/安装成功（产出 `rknn_yolov8_demo` 与 `rknn_yolov8_demo_zero_copy`）
- 仅提示安装目录下缺少 `.rknn` 模型文件（属于模型资产问题，不是工具链问题）

### 2.4 官方参考链路已覆盖目标
可直接对照的官方示例：
- `.../experiments/rknn_toolkit2/rknpu2/examples/rknn_yolov5_demo/src/main_video.cc`

其流程为：
`MPP decode -> RGA resize/色转 -> RKNN 推理 -> YUV420SP 画框 -> MPP encode`

与本项目目标高度一致，可作为实现顺序与风险边界参考。

### 2.5 手册约束已确认
- MPP：外部缓冲模式、info-change 重配置、`encode_put_frame` 阻塞语义
  - 参考：`/home/joe/Code/V4L2-Develop/Basic_WthoutAI/experiments/mpp_src_try/mpp/doc/Rockchip_Developer_Guide_MPP_CN.md`
- RGA：`imresize/imcvtcolor` 与 `imrectangle/imrectangleArray`
  - 参考：`/home/joe/Documents/DevelopDoc/Rockchip_Developer_Guide_RGA_CN.md`

## 3. 技术路线与决策（默认推荐）

在未收到你进一步偏好前，本方案采用“低风险、可尽快上线”的默认路线：

1. 模型：`YOLOv8n int8`（RKNN）作为首版基线
2. 推理输入：`NV12 -> RGA 转 RGB + resize/letterbox -> RKNN`（避免强依赖模型直接吃 NV12）
3. OSD：优先使用 `RGA imrectangleArray` 在编码输入 NV12 上画框
4. 线程：先复用现有 RGA 处理线程串行接入推理与画框，减少并发改造风险
5. 推流：保持现有编码输出到 ZLM 的 RTSP/WebRTC 通路不变

## 4. 目标架构（集成后）

### 4.1 数据流
1. `decode_thread` 产出 `decoded_output(IO_FD_t*)`
2. `rga_thread` 从 `rgaConsumeQueue` 取帧
3. `rga_thread` 内执行：
   - `RGA copy/convert` 得到编码输入 NV12 buffer（来自 `RgaInstance::output_pool_`）
   - `RGA resize + color convert` 得到 NPU 输入 RGB buffer
   - `RKNN run + postprocess` 得到框集合
   - `RGA imrectangleArray` 在编码输入 NV12 buffer 画框
4. 将画框后的 NV12 buffer 推入 `mppEncodeInputQueue`
5. `mpp_encode_input_thread` 调 `encode_put_frame`
6. `mpp_encode_output_thread` 出 H264 AU，继续送 ZLM

### 4.2 内存策略
- 编码输入仍使用现有 RGA 输出池 DMABUF（保持现有稳定路径）
- NPU 输入新增独立 DMABUF 池（建议 3~4 个）
- NPU 前处理输出写入 NPU 输入池，再用 `rknn_set_io_mem`/`rknn_run`
- 检测框结果仅为轻量 CPU 元数据（vector）

### 4.3 生命周期与所有权
- `decoded_output` 生命周期：由解码器和回收线程管理，不直接改写
- `rga_output`（编码输入）生命周期：RGA线程产生 -> 编码线程消费 -> 回收到 RGA 池
- NPU 输入池生命周期：`RknnInferInstance` 持有并在析构释放

## 5. 代码改造清单（决策完备）

### 5.1 新增模块
1. `include/rknn_infer.h`
2. `src/rknn_infer.cpp`

职责：
- 模型加载/销毁（`rknn_init/rknn_destroy`）
- 输入输出 attr 查询和缓存
- NPU 输入池管理（fd + virt）
- 单帧推理接口 `InferFrame(const IO_FD_t* nv12_src, DetectionResultList* out)`

### 5.2 新增公共类型
在 `include/pubDataType.h` 增加：
- `BBox`（left/top/right/bottom/conf/class_id）
- `DetectionResultList`（固定上限 + count）

说明：不把检测结果塞进 `IO_FD_t`，避免污染通用 DMA 描述结构。

### 5.3 RGA 模块扩展
在 `include/rkrga.h` / `src/rkrga.cpp` 增加：
- `int ResizeToRgb(const IO_FD_t* src_nv12, NpuInputBuffer* dst_rgb)`
- `int DrawRectanglesNv12(const IO_FD_t* nv12_dst, const DetectionResultList& dets)`

实现基于：
- `imresize` / `imcvtcolor`
- `imrectangleArray`

### 5.4 main 流水线改造
`src/main.cpp` 中仅改造 `run_rga_thread`：
1. 现有 `Copy(input_desc)` 逻辑保留（保证编码输入路径连续）
2. 在获得 `rga_output` 后调用 `rknn_infer.InferFrame(input_desc, &dets)`
3. 推理成功则 `DrawRectanglesNv12(rga_output, dets)`
4. 推理失败时执行降级：不画框直接编码（不中断主流程）

### 5.5 CMake 改造
`CMakeLists.txt` 增加：
- 新源文件 `src/rknn_infer.cpp`
- RKNN 头文件路径（例如 `experiments/rknn_toolkit2/rknpu2/runtime/Linux/librknn_api/include`）
- 链接 `rknnrt`（优先系统 `/lib/librknnrt.so`，允许后续切 install 目录）

## 6. 关键可行性与风险分析

### 6.1 可行性结论
- 从手册、官方 video demo、现有代码结构三者交叉验证，方案可行。
- 主要风险不在“能不能跑”，而在“实时性与阻塞传播”。

### 6.2 主要风险
1. 单线程串行（RGA+NPU）可能拉低峰值 FPS
2. 推理耗时抖动导致 `mppEncodeInputQueue` 积压
3. OSD 绘制坐标与 stride/letterbox 映射错误导致框偏移
4. info-change（分辨率变化）时 NPU/RGA 资源未同步重建

### 6.3 对应缓解
1. 首版先串行稳定，二期再拆独立 NPU 线程
2. 增加队列水位与丢帧策略（高水位丢旧保新）
3. 固化坐标变换单元测试（含边界值）
4. 在检测到 info-change 后触发 NPU/RGA buffer 重建

## 7. 验收标准（硬指标）

### 7.1 功能验收
1. RTSP 可稳定播放含框视频 ≥ 30 分钟
2. WebRTC 可稳定播放含框视频 ≥ 30 分钟
3. 编码输出持续为 H264，关键帧与时间戳连续

### 7.2 性能验收（RK3588，1080p 输入，YOLOv8n int8）
1. 端到端输出帧率不低于输入帧率的 85%
2. 单帧额外推理+OSD 开销 P95 < 25ms
3. 长稳（30 分钟）无崩溃、无持续内存增长

### 7.3 鲁棒性验收
1. NPU 临时失败时自动降级为“无框编码推流”
2. 分辨率变化后可自动恢复推理与画框
3. 推流端临时断连不导致采集解码线程崩溃

## 8. 详细实施步骤（按阶段）

### 阶段 A：NPU 最小闭环（离线单帧）
1. 新增 `RknnInferInstance`，完成模型加载与 `InferFrame`
2. 用仓库中的单帧 NV12 测试输入验证推理输出框数量与置信度范围
3. 打印每阶段耗时：预处理、rknn_run、后处理

退出条件：单帧连续调用 1000 次无泄漏/无崩溃。

### 阶段 B：在线串流接入（不画框）
1. 在 `run_rga_thread` 中接入推理调用
2. 推理失败降级编码，不影响现有推流
3. 加入统计：NPU 平均耗时、失败计数、队列长度

退出条件：RTSP/WebRTC 都可持续播放，且 pipeline 不中断。

### 阶段 C：在线 OSD 画框
1. 接入 `imrectangleArray` 在编码输入 NV12 上绘制
2. 校验 letterbox 坐标反变换与边界裁剪
3. 验证框与目标一致，无明显偏移

退出条件：可稳定输出带框视频。

### 阶段 D：性能调优
1. 调整输入尺寸（640/512）与阈值
2. 根据队列水位启用丢帧策略
3. 必要时拆分独立 NPU 线程并增加缓冲深度

退出条件：达到第 7 章性能指标。

## 9. 测试矩阵

1. 分辨率：1280x720 / 1920x1080
2. 光照与场景：明亮/低照/快速运动
3. 模型：yolov8n int8（首版）
4. 推流协议：RTSP / WebRTC
5. 压测时长：5min / 30min / 2h
6. 异常注入：
   - 暂停 NPU 调用
   - ZLM 断开重连
   - 分辨率切换

## 10. 观测与日志

建议新增周期日志（每 300 帧）：
- `fps_in / fps_out`
- `npu_ms_avg / npu_ms_p95`
- `rga_ms_avg`
- `encode_queue_depth`
- `infer_fail_count / osd_fail_count`
- `drop_count`

## 11. 默认参数（首版）

1. 模型输入：640x640 RGB
2. 阈值：`conf=0.25`，`nms=0.45`
3. 框颜色：红色，线宽 3
4. 队列高水位：`mppEncodeInputQueue > 8` 开始丢旧帧
5. 推理失败降级：保留编码推流，不阻塞主链路

## 12. 后续可选增强

1. 将推理线程从 RGA 线程拆分，提高流水并行度
2. 使用 RKNN 零拷贝输入输出进一步降低拷贝开销
3. 支持多模型切换与热加载
4. 增加 OSD 文本（类别+置信度）

## 13. 关键参考路径

- 主流程：`/home/joe/Code/V4L2-Develop/Basic_WthoutAI/src/main.cpp`
- 解码（模式3）：`/home/joe/Code/V4L2-Develop/Basic_WthoutAI/src/rkmppdec.cpp`
- 编码：`/home/joe/Code/V4L2-Develop/Basic_WthoutAI/src/rkmppenc.cpp`
- RGA：`/home/joe/Code/V4L2-Develop/Basic_WthoutAI/src/rkrga.cpp`
- 官方视频链路样例：`/home/joe/Code/V4L2-Develop/Basic_WthoutAI/experiments/rknn_toolkit2/rknpu2/examples/rknn_yolov5_demo/src/main_video.cc`
- RKNN API：`/home/joe/Code/V4L2-Develop/Basic_WthoutAI/experiments/rknn_toolkit2/rknpu2/runtime/Linux/librknn_api/include/rknn_api.h`
- MPP 手册：`/home/joe/Code/V4L2-Develop/Basic_WthoutAI/experiments/mpp_src_try/mpp/doc/Rockchip_Developer_Guide_MPP_CN.md`
- RGA 手册：`/home/joe/Documents/DevelopDoc/Rockchip_Developer_Guide_RGA_CN.md`

---

## 附：关于“同架构本机编译”的结论
`build-linux.sh` 不是“自动识别并切 native gcc”，而是默认使用交叉前缀（如 `aarch64-linux-gnu-gcc`）。
在你的本机上，这个编译器已经可用，因此脚本可直接成功执行。也就是说：
- 不是脚本智能适配了 native
- 而是你当前系统恰好具备它要求的工具链，所以可直接编译
