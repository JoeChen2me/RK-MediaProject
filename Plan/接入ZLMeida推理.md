# MPP 编码输出接入 ZLM 完整方案（直连输出线程 + splitter + MPP dts/pts）

## 摘要
本方案在不改变相机/解码/RGA/编码输入链路的前提下，补齐编码输出到 ZLM 的发布能力。  
已锁定决策如下：
1. MPP 到 ZLM 采用“编码输出线程直喂”，不新增中转线程。
2. H264 数据“全量走 `mk_h264_splitter`”处理分片/粘包。
3. 时间戳使用 MPP 输出 `dts/pts`，并做严格兜底与单调修正。
4. 风格对齐现有类：`Init/Close`、`int(0/-1)` 返回、不抛异常、资源在析构释放。

## 问题结论（先回答你当前关注）
1. `packet -> vector` 拷贝开销是否大：默认码率 2Mbps 下，单次 memcpy 的数据吞吐仅约 0.25MB/s，CPU 开销很低；即便到 20Mbps 也约 2.5MB/s，通常不是瓶颈。  
2. 但为了降低不必要拷贝，本方案不在 `MppEncInstance` 内做 `vector` 封装，仅在 `ZlmPublisher` 内做“一次可复用 scratch 缓冲拷贝”（用于满足 splitter 对 `len+1` 可写内存要求）。  
3. splitter 路径可行且和官方一致：`mk_h264_splitter_create` + 回调 + `mk_media_input_frame`；对 MPP 分片包不需要手工组 AU。  
4. “更方便传递”采用“编码线程直接调用 `ZlmPublisher::InputPacket`”，减少中间队列与 ownership 复杂度。

## 对外接口/类型变更
1. 新增 `include/zlm_publisher.h` 与 `src/zlm_publisher.cpp`。  
2. 新增类 `ZlmPublisher`，固定接口：
   - `int Init();`
   - `int InputExtraData(const uint8_t* data, size_t len);`
   - `int InputPacket(const uint8_t* data, size_t len, int64_t dts_ms, int64_t pts_ms, bool eos);`
   - `void Close();`
3. 调整 `include/rkmppenc.h`：
   - 新增 `struct EncodedPacketView`：`const void* data`、`size_t len`、`int64_t dts_ms`、`int64_t pts_ms`、`bool eos`、`MppPacket handle`。
   - `EncoderGetPacket` 改为：`int EncoderGetPacket(EncodedPacketView* out_pkt);`
   - 新增 `int EncoderReleasePacket(EncodedPacketView* pkt);`
   - 新增 `int GetExtraInfo(std::vector<uint8_t>* out_extra);`
4. 保留 `IsPacketEOS()` 语义不变，继续用于线程退出判断。

## 详细实现（按文件）
### 1) `src/rkmppenc.cpp`
1. 修改 `BuildInputFrameFromFd` 的输入 PTS 为毫秒：`pts_ms = frame_id * 1000 / fps`，避免后续 ZLM 时间戳单位不一致。  
2. `EncoderGetPacket(EncodedPacketView*)` 行为改为“取一个 packet chunk，不在内部 deinit”：
   - `ret == MPP_ERR_TIMEOUT` 或 `packet == nullptr` 返回 `kNoPacket`。
   - 成功时填写 `out_pkt`：`data/len/dts/pts/eos/handle`。
   - 若 `eos`，置 `ReachPacketEOS_ = true`。
   - 不再在此函数内 `mpp_packet_deinit`。
3. 新增 `EncoderReleasePacket(EncodedPacketView*)`：
   - 对 `handle` 执行 `mpp_packet_deinit`，并清空 view 字段，保证调用幂等。
4. 新增 `GetExtraInfo(std::vector<uint8_t>*)`：
   - 通过 `MPP_ENC_GET_EXTRA_INFO` 获取 SPS/PPS。
   - 一次性复制到 `vector`（仅初始化阶段一次，开销可忽略）。
   - 失败返回 `-1`。

### 2) `include/zlm_publisher.h` / `src/zlm_publisher.cpp`
1. 资源成员：
   - `mk_media media_`
   - `mk_track video_track_`
   - `mk_h264_splitter splitter_`
   - `std::vector<char> splitter_input_buf_`（可复用，满足 `len+1`）
   - `std::deque<TimestampItem> ts_queue_`
   - `int64_t last_dts_ms_ / last_pts_ms_`
2. `Init()` 固定流程：
   - `mk_env_init`
   - `mk_rtsp_server_start(8554, 0)`
   - `mk_media_create("__defaultVhost__", "live", "camera", 0, 0, 0)`
   - `mk_track_create(MKCodecH264, &codec_args{})`
   - `mk_media_init_track`
   - `mk_media_init_complete`
   - `mk_h264_splitter_create(on_h264_frame, this, 0)`
3. `InputExtraData()`：
   - Annex-B 校验通过后喂 `mk_h264_splitter_input_data`。
   - 不入时间戳队列。
4. `InputPacket()`：
   - 校验 `data/len` 与 Annex-B 起始码。
   - 将 `dts/pts` 按“严格MPP+兜底”修正后入 `ts_queue_`。
   - 将 packet 拷贝到 `splitter_input_buf_`（长度 `len+1`），末尾写 `'\0'`，再喂 splitter。
5. splitter 回调 `on_h264_frame`：
   - 从 `ts_queue_` 取一组时间戳；若队列空，使用 `last_dts_ms_ + frame_interval_ms` 兜底。
   - `mk_frame_create(MKCodecH264, dts, pts, frame, size, NULL, NULL)`
   - `mk_media_input_frame(media_, frame_obj)` 后 `mk_frame_unref(frame_obj)`。
6. `Close()` 顺序：
   - 先 `mk_h264_splitter_release`（触发残留 flush）
   - 再 `mk_media_release`
   - 最后 `mk_stop_all_server`

### 3) `src/main.cpp`
1. 新增 `ZlmPublisher zlm_publisher;`，在编码器初始化后调用 `zlm_publisher.Init()`。  
2. 编码器配置完成后，调用 `mpp_encoder_instance.GetExtraInfo(&extra)`；成功则 `zlm_publisher.InputExtraData(extra.data(), extra.size())`。  
3. 改造 `mpp_encode_output_thread`：
   - 调用 `EncoderGetPacket(&pkt_view)`。
   - `kHasPacket` 时直接 `zlm_publisher.InputPacket(...)`。
   - 无论喂流成功与否，必须调用 `EncoderReleasePacket(&pkt_view)`。
4. 退出流程：
   - 线程 join 完成后调用 `zlm_publisher.Close()`。
5. 统计项（每 5 秒打印一次，禁止逐帧日志）：
   - `enc_packet_out_total`
   - `zlm_input_ok`
   - `zlm_input_fail`
   - `zlm_splitter_out_frame`
   - `zlm_timestamp_fallback_count`

### 4) `CMakeLists.txt`
1. 将 `src/zlm_publisher.cpp` 加入 `app` 源文件列表。  
2. 现有 `include/ZLM` 与 `libmk_api.so/libZLToolKit.so` 链接保持不变。  

## 时间戳规则（决策固定）
1. 优先用 `mpp_packet_get_dts/pts`。  
2. 若 `dts < 0`，用 `pts`；若 `pts < 0`，用 `dts`。  
3. 两者仍无效时，回落 `last_dts + frame_interval_ms`。  
4. 强制单调：`dts <= last_dts` 时改为 `last_dts + 1`。  
5. 保证 `pts >= dts`。  

## Annex-B 与分片处理规则
1. 输入到 splitter 前必须做起始码检查（`00 00 01` 或 `00 00 00 01`）。  
2. 检查失败直接计数并丢弃，不送 ZLM。  
3. 全量走 splitter，不再依赖 EOI 手工聚合路径。  

## 测试用例与验收标准
1. 构建：`cmake -S . -B build && cmake --build build -j` 成功。  
2. 功能：启动后可拉流 `rtsp://<ip>:8554/live/camera`。  
3. 稳定性：连续运行 30 分钟无线程卡死、无资源泄漏报错。  
4. 指标：
   - `enc_packet_out_total > 0`
   - `zlm_input_ok > 0`
   - `zlm_input_fail / (zlm_input_ok + zlm_input_fail) <= 1%`
   - `zlm_timestamp_fallback_count` 应接近 0（偶发可接受）  
5. 分片兼容回归：临时启用编码 split 配置后，仍可连续播放，`zlm_splitter_out_frame` 持续增长。  
6. 退出：Ctrl+C 后所有线程退出，`Close()` 顺序执行，无重复释放异常。  

## 假设与默认值
1. 当前仅接 H264（不含音频）。  
2. MPP 输出为 Annex-B（与手册一致）；若异常则按校验丢弃并计数。  
3. 发布模式为本地 RTSP，固定 `__defaultVhost__/live/camera` 与端口 `8554`。  
4. `ZlmPublisher::InputPacket` 仅由编码输出线程调用（单线程约束）。  
