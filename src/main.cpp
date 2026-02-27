#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rkmppdec.h"
#include "rkrga.h"
#include "v4l2Camera.h"
#include "rkmppenc.h"
#include "zlm_publisher.h"

const std::string            device      = "/dev/video0";  // USB 摄像头路径
static volatile sig_atomic_t should_exit = 0;              // 用于控制程序退出的标志

static void signal_handler(int signum)
{
    if (signum == SIGINT)
    {
        should_exit = 1;
    }
}

int main()
{
    // 注册 SIGINT 信号处理函数，以便在按下 Ctrl+C 时能够优雅地退出程序
    std::signal(SIGINT, signal_handler);

    std::cout << "[CFG] Fixed pipeline: CAMERA_IO_MODE=dmabuf, MPP_DEC_INPUT_MODE=zerocopy"
              << std::endl;

    V4L2_Camera camera;
    if (camera.camera_GlobalInit(device, 20) != 0)  // 全局初始化，设置曝光时间为 20ms
    {
        std::cerr << "Failed to initialize camera globally" << std::endl;
        return 1;
    }
    const VideoCaptureConfig& active_cfg = camera.get_active_config();
    if (!active_cfg.valid)
    {
        std::cerr << "Camera active config is invalid after initialization" << std::endl;
        return 1;
    }

    MppDecInstance mpp_decoder_instance;
    if (mpp_decoder_instance.DecInit() != 0)  // 初始化 MPP 实例
    {
        std::cerr << "Failed to initialize MPP instance" << std::endl;
        return 1;
    }
    if (mpp_decoder_instance.DecConfigWidthHeight(active_cfg.width, active_cfg.height) !=
        0)  // 配置 MPP 的宽高参数
    {
        std::cerr << "Failed to configure MPP width and height" << std::endl;
        return 1;
    }
    if (mpp_decoder_instance.DecAllocBuffer(camera.FrameDescArray,
                                            static_cast<size_t>(camera.get_buffer_count())) != 0)
    {
        std::cerr << "Failed to import V4L2 dma-buf into MPP input buffers" << std::endl;
        return 1;
    }

    RgaInstance rga_instance;
    if (rga_instance.RgaInit() != 0)
    {
        std::cerr << "Failed to initialize RGA instance" << std::endl;
        return 1;
    }

    MppEncInstance mpp_encoder_instance;
    if (mpp_encoder_instance.EncInit() != 0)
    {
        std::cerr << "Failed to initialize MPP encoder instance" << std::endl;
        return 1;
    }
    if (mpp_encoder_instance.EncConfigWidthHeight(active_cfg.width, active_cfg.height, 0, 0) != 0)
    {
        std::cerr << "Failed to configure MPP encoder width and height" << std::endl;
        return 1;
    }

    ZlmPublisher zlm_publisher;
    if (zlm_publisher.Init() != 0)
    {
        std::cerr << "Failed to initialize ZLM publisher" << std::endl;
        return 1;
    }

    std::atomic_uint64_t enc_pkt_total{0};
    std::atomic_uint64_t zlm_input_ok{0};
    std::atomic_uint64_t zlm_input_fail{0};

    std::mutex ready_mutex;        // 保护 readyQueue 的互斥锁 相机->解码线程
    std::mutex recycle_mutex;      // 保护 recycleQueue 的互斥锁 解码线程->相机
    std::mutex rga_consume_mutex;  // 保护 rgaConsumeQueue 的互斥锁 解码线程->RGA 线程
    std::mutex mpp_recycle_output_mutex;  // 保护 mppRecycleOutputQueue 的互斥锁 RGA 线程->解码线程
    std::mutex mpp_encode_input_mutex;  // 保护 mppEncodeInputQueue 的互斥锁 RGA 线程->编码线程
    std::condition_variable ready_cv;  // 用于通知解码线程有新帧可处理的条件变量
    std::condition_variable rga_consume_cv;  // 用于通知 RGA 线程有新帧可处理的条件变量
    std::condition_variable mpp_recycle_output_cv;  // 用于通知解码输出可回收线程有新数据
    std::condition_variable mpp_encode_input_cv;  // 用于通知编码输入线程有新帧可处理
    std::deque<FrameDesc*>  readyQueue;  // 存储已捕获但未解码的帧描述指针的队列
    std::deque<FrameDesc*> recycleQueue;  /// 存储已处理完毕、等待回收的帧描述指针的队列
    std::deque<const IO_FD_t*> rgaConsumeQueue;  // 存储已解码但未 RGA 处理的帧描述指针的队列
    std::deque<const IO_FD_t*>
        mppRecycleOutputQueue;  // 存储已 RGA 处理但未回收的输出描述指针的队列
    std::deque<const IO_FD_t*> mppEncodeInputQueue;  // 存储已 RGA 处理但未送编码的帧描述指针的队列

    // 开关
    std::atomic_bool stop_requested{false};
    std::atomic_bool fatal_error{false};
    std::atomic_bool rga_thread_done{false};  // RGA线程退出标志  其退出后回收线程再退出
    std::atomic_bool encode_input_done{false};
    // 统计信息
    size_t           dropped_frames = 0;
    constexpr size_t kMaxReadyQueueDepth =
        resource_limits::kCameraRequestBufferCount * 0.75;  // 控制端到端延迟，超限时丢弃最旧帧
    constexpr auto kRetryBackoffSleep = std::chrono::milliseconds(2);  // 重试退避，避免短时间忙轮询
    constexpr auto kEncodeNoPacketSleep =
        std::chrono::milliseconds(30);  // 无包时退避，避免编码输出线程忙等抢占 CPU
    constexpr auto kMainLoopSleep = std::chrono::milliseconds(20);  // 主线程轮询退出信号间隔

    auto request_stop = [&]()
    {
        stop_requested.store(true);
        ready_cv.notify_all();
        rga_consume_cv.notify_all();
        mpp_recycle_output_cv.notify_all();
        mpp_encode_input_cv.notify_all();
    };

    auto drain_mpp_recycle_requests = [&]()
    {
        std::deque<const IO_FD_t*> pending_outputs;
        {
            std::lock_guard<std::mutex> lock(mpp_recycle_output_mutex);
            while (!mppRecycleOutputQueue.empty())
            {
                pending_outputs.push_back(mppRecycleOutputQueue.front());
                mppRecycleOutputQueue.pop_front();
            }
        }

        for (const IO_FD_t* output_desc : pending_outputs)
        {
            if (mpp_decoder_instance.DecQueueOutputForRecycle(output_desc) != 0)
            {
                std::cerr << "Failed to queue decoded output for recycle, fd="
                          << ((output_desc) ? output_desc->fd : -1) << std::endl;
            }
        }
    };

    std::thread capture_thread(
        [&]()
        {
            if (camera.get_buffer_count() <= 0)
            {
                std::cerr << "Invalid camera buffer count: 0" << std::endl;
                fatal_error.store(true);
                request_stop();
                ready_cv.notify_all();
                return;
            }

            while (!stop_requested.load())
            {
                std::deque<FrameDesc*> recycled_batch;
                FrameDesc*             dropped_desc = nullptr;
                {
                    // 这里扩起来是为了减少持锁时间，先取出要回收的缓冲区指针，等到锁外再执行回收操作，避免在可能的
                    // I/O 操作中持锁。
                    std::lock_guard<std::mutex> lock(recycle_mutex);
                    while (!recycleQueue.empty())
                    {
                        recycled_batch.push_back(recycleQueue.front());
                        recycleQueue.pop_front();
                    }
                }

                for (FrameDesc* recycled_desc : recycled_batch)
                {
                    if (recycled_desc && camera.requeue_buffer(recycled_desc) != 0)
                    {
                        std::cerr << "Failed to requeue recycled buffer in capture thread"
                                  << std::endl;
                        fatal_error.store(true);
                        request_stop();
                        break;
                    }
                }
                if (stop_requested.load())
                {
                    break;
                }

                size_t frame_length = camera_params::kMaxFrameSize;
                int    frame_capture_ret =
                    camera.camera_read_frame(&frame_length, camera_params::kMaxFrameSize);
                if (frame_capture_ret == camera_read_result::kRetryable)
                {
                    std::this_thread::sleep_for(kRetryBackoffSleep);
                    continue;
                }
                if (frame_capture_ret == camera_read_result::kTimeout)
                {
                    std::cerr << "Camera dequeue timeout reached ("
                              << camera_params::kDequeueTimeoutMs << " ms), stop capture thread"
                              << std::endl;
                    fatal_error.store(true);
                    request_stop();
                    break;
                }
                if (frame_capture_ret != camera_read_result::kOk)
                {
                    std::cerr << "Failed to read a frame from the camera" << std::endl;
                    fatal_error.store(true);
                    request_stop();
                    break;
                }

                FrameDesc* frame_desc = camera.CurrentFrameDesc;
                if (frame_desc == nullptr || frame_desc->index < 0)
                {
                    std::cerr << "Invalid current frame descriptor after capture" << std::endl;
                    fatal_error.store(true);
                    request_stop();
                    break;
                }

                {
                    // 将新捕获的帧描述加入 readyQueue，准备被解码线程处理。
                    // 扩起来是为了减小持锁作用域，先在锁内检查并更新 readyQueue
                    // 的状态，决定是否需要丢弃最旧的帧，然后在锁外执行具体的入队和丢弃操作。
                    std::lock_guard<std::mutex> lock(ready_mutex);
                    if (readyQueue.size() >= kMaxReadyQueueDepth)
                    {
                        dropped_desc = readyQueue.front();
                        readyQueue.pop_front();
                    }
                    readyQueue.push_back(frame_desc);
                }

                if (dropped_desc)
                {
                    {
                        // 满队列时丢弃最旧帧，但不在这里立即回队；
                        // 统一交给采集线程下一轮的 recycleQueue 批量回收流程处理。
                        std::lock_guard<std::mutex> lock(recycle_mutex);
                        recycleQueue.push_back(dropped_desc);
                    }
                    ++dropped_frames;
                }
                ready_cv.notify_one();
            }

            ready_cv.notify_all();
        });

    std::thread decode_thread(
        [&]()
        {
            while (true)
            {
                FrameDesc* frame_desc = nullptr;
                {
                    std::unique_lock<std::mutex> lock(ready_mutex);
                    ready_cv.wait(lock,
                                  [&]() { return !readyQueue.empty() || stop_requested.load(); });
                    if (readyQueue.empty())
                    {
                        if (stop_requested.load())
                        {
                            break;
                        }
                        continue;
                    }
                    frame_desc = readyQueue.front();
                    readyQueue.pop_front();
                }

                if (frame_desc == nullptr)
                {
                    std::cerr << "Ready queue returned null frame descriptor" << std::endl;
                    fatal_error.store(true);
                    request_stop();
                    continue;
                }

                if (mpp_decoder_instance.MppDecode(frame_desc) != 0)
                {
                    // 解码失败直接跳过该帧，随后由 recycleQueue 回收到相机，保证采集链路继续前进。
                    std::cerr << "Failed to decode frame, index=" << frame_desc->index
                              << ", payload=" << frame_desc->payloadSize << std::endl;
                }
                else
                {
                    const IO_FD_t* decoded_output = mpp_decoder_instance.CurrentOutputDesc;
                    if (!decoded_output || decoded_output->width == 0 ||
                        decoded_output->height == 0 || decoded_output->hor_stride == 0 ||
                        decoded_output->ver_stride == 0)
                    {
                        std::cerr << "Decoded output metadata is invalid" << std::endl;
                        if (decoded_output &&
                            mpp_decoder_instance.DecQueueOutputForRecycle(decoded_output) != 0)
                        {
                            std::cerr << "Failed to recycle invalid decoded output, fd="
                                      << decoded_output->fd << std::endl;
                        }
                    }
                    else
                    {
                        {
                            std::lock_guard<std::mutex> lock(rga_consume_mutex);
                            rgaConsumeQueue.push_back(decoded_output);
                        }
                        rga_consume_cv.notify_one();
                    }
                }

                {
                    std::lock_guard<std::mutex> lock(recycle_mutex);
                    recycleQueue.push_back(frame_desc);
                }
            }
        });

    std::thread mpp_output_recycle_thread(
        [&]()
        {
            while (true)
            {
                std::deque<const IO_FD_t*> pending_outputs;
                {
                    std::unique_lock<std::mutex> lock(mpp_recycle_output_mutex);
                    mpp_recycle_output_cv.wait(
                        lock,
                        [&]() {
                            return !mppRecycleOutputQueue.empty() ||
                                   (stop_requested.load() && rga_thread_done.load());
                        });
                    while (!mppRecycleOutputQueue.empty())
                    {
                        pending_outputs.push_back(mppRecycleOutputQueue.front());
                        mppRecycleOutputQueue.pop_front();
                    }
                    if (pending_outputs.empty() && stop_requested.load() && rga_thread_done.load())
                    {
                        break;
                    }
                }

                for (const IO_FD_t* output_desc : pending_outputs)
                {
                    if (mpp_decoder_instance.DecQueueOutputForRecycle(output_desc) != 0)
                    {
                        std::cerr << "Failed to queue decoded output for recycle, fd="
                                  << ((output_desc) ? output_desc->fd : -1) << std::endl;
                    }
                }
            }
        });

    std::thread rga_thread(
        [&]()
        {
            constexpr size_t kMaxRgaTransformRetry = 2000;  // 约 4 秒重试窗口
            size_t           rga_frame_count       = 0;
            bool             encoder_io_ready      = false;
            while (true)
            {
                const IO_FD_t* input_desc = nullptr;
                {
                    std::unique_lock<std::mutex> lock(rga_consume_mutex);  // 获取锁
                    rga_consume_cv.wait(
                        lock, [&]() { return !rgaConsumeQueue.empty() || stop_requested.load(); });
                    if (rgaConsumeQueue.empty())
                    {
                        if (stop_requested.load())
                        {
                            break;
                        }
                        continue;
                    }
                    input_desc = rgaConsumeQueue.front();  // 有数据了，取出队首元素
                    rgaConsumeQueue.pop_front();           // 从队列中移除已取出的元素
                }

                if (!input_desc)
                {
                    continue;
                }

                const IO_FD_t* rga_output      = nullptr;
                size_t         rga_retry_count = 0;
                while (!stop_requested.load())
                {
                    rga_output = rga_instance.Copy(input_desc);
                    if (rga_output != nullptr)
                    {
                        break;
                    }

                    rga_retry_count++;
                    if (rga_retry_count >= kMaxRgaTransformRetry)
                    {
                        std::cerr << "RGA processing failed repeatedly, fd=" << input_desc->fd
                                  << ", retry_count=" << rga_retry_count << std::endl;
                        fatal_error.store(true);
                        request_stop();
                        break;
                    }
                    std::this_thread::sleep_for(kRetryBackoffSleep);
                }

                if (rga_output != nullptr)
                {
                    if (!encoder_io_ready)
                    {
                        if (mpp_encoder_instance.AllocBufferForIO(
                                rga_instance.output_pool_,
                                resource_limits::kRgaOutputBufferCount) != 0)
                        {
                            std::cerr << "Failed to import RGA output buffers for encoder"
                                      << std::endl;
                            (void)rga_instance.QueueOutputToRecycle(rga_output);
                            fatal_error.store(true);
                            request_stop();
                        }
                        else
                        {
                            encoder_io_ready = true;
                        }
                    }
                    if (encoder_io_ready && !stop_requested.load() && rga_output != nullptr)
                    {
                        {
                            std::lock_guard<std::mutex> lock(mpp_encode_input_mutex);
                            mppEncodeInputQueue.push_back(rga_output);  // 压入MPP编码输入队列
                        }
                        mpp_encode_input_cv.notify_one();
                    }
                }

                {
                    std::lock_guard<std::mutex> lock(mpp_recycle_output_mutex);  // 获取锁
                    mppRecycleOutputQueue.push_back(
                        input_desc);  // 将处理完成的输出描述加入MPP 解码器回收队列
                }
                mpp_recycle_output_cv.notify_one();  // 通知解码器回收线程有新输出可回收
                rga_frame_count++;
                if ((rga_frame_count % 300u) == 0u)
                {
                    std::cout << "RGA processed frames: " << rga_frame_count
                              << ", last_input_fd=" << input_desc->fd << std::endl;
                }
            }
            rga_thread_done.store(true);
            mpp_recycle_output_cv.notify_all();
        });

    std::thread mpp_encode_input_thread(
        [&]()
        {
            constexpr size_t kMaxEncodePushRetry  = 200;
            size_t           encode_push_failures = 0;
            while (true)
            {
                const IO_FD_t* encode_input_desc = nullptr;
                {
                    std::unique_lock<std::mutex> lock(mpp_encode_input_mutex);
                    mpp_encode_input_cv.wait(
                        lock,
                        [&]() { return !mppEncodeInputQueue.empty() || stop_requested.load(); });
                    if (mppEncodeInputQueue.empty())
                    {
                        if (stop_requested.load())
                        {
                            break;
                        }
                        continue;
                    }
                    encode_input_desc = mppEncodeInputQueue.front();
                    mppEncodeInputQueue.pop_front();
                }

                if (!encode_input_desc)
                {
                    continue;
                }
                if (stop_requested.load())
                {
                    (void)rga_instance.QueueOutputToRecycle(encode_input_desc);
                    break;
                }
                // 将 RGA 处理后的输出送入编码器，若失败则重试，重试超过阈值则记录错误并停止程序。
                if (mpp_encoder_instance.EncodePushFrame(encode_input_desc) != 0)
                {
                    if (stop_requested.load())
                    {
                        (void)rga_instance.QueueOutputToRecycle(encode_input_desc);
                        break;
                    }
                    encode_push_failures++;
                    if (encode_push_failures <= kMaxEncodePushRetry)
                    {
                        if ((encode_push_failures % 20u) == 1u)
                        {
                            std::cerr
                                << "Encode push temporary failure, fd=" << encode_input_desc->fd
                                << ", retry=" << encode_push_failures << std::endl;
                        }
                        {
                            std::lock_guard<std::mutex> lock(mpp_encode_input_mutex);
                            mppEncodeInputQueue.push_front(
                                encode_input_desc);  // 回队重试，避免瞬时失败直接中断流程
                        }
                        mpp_encode_input_cv.notify_one();
                        std::this_thread::sleep_for(kRetryBackoffSleep);  // 退避
                        continue;
                    }

                    std::cerr << "Failed to push frame to encoder after retries, fd="
                              << encode_input_desc->fd << ", retry=" << encode_push_failures
                              << std::endl;
                    (void)rga_instance.QueueOutputToRecycle(
                        encode_input_desc);  // 重试后仍失败，回收该输出避免资源泄漏
                    fatal_error.store(true);
                    request_stop();
                    break;
                }

                encode_push_failures = 0;
                // encode_put_frame 为阻塞式，返回时输入帧已被编码器消费，可立即回收。
                if (rga_instance.QueueOutputToRecycle(encode_input_desc) != 0)
                {
                    std::cerr << "Failed to recycle RGA output after encode_put_frame, fd="
                              << encode_input_desc->fd << std::endl;
                    fatal_error.store(true);
                    request_stop();
                    break;
                }
            }
            encode_input_done.store(true);
        });

    std::thread mpp_encode_output_thread(
        [&]()
        {
            auto                 last_stat_tp = std::chrono::steady_clock::now();
            std::vector<uint8_t> pending_au;
            int64_t              pending_dts_ms   = -1;
            int64_t              pending_pts_ms   = -1;
            bool                 pending_eos      = false;
            auto                 flush_pending_au = [&]()
            {
                if (pending_au.empty())
                {
                    return;
                }
                EncPacketView au_view;
                au_view.data         = pending_au.data();
                au_view.len          = pending_au.size();
                au_view.dts_ms       = pending_dts_ms;
                au_view.pts_ms       = pending_pts_ms;
                au_view.eos          = pending_eos;
                au_view.is_partition = false;
                au_view.is_eoi       = true;
                au_view.is_extra     = false;
                au_view.handle       = nullptr;

                if (zlm_publisher.InputPacketChunk(au_view) == 0)
                {
                    zlm_input_ok.fetch_add(
                        1, std::memory_order_relaxed);  // 以原子方式将指定值加到当前变量上
                }
                else
                {
                    zlm_input_fail.fetch_add(1, std::memory_order_relaxed);
                    std::cerr << "Failed to input encoded AU to ZLM, len=" << au_view.len
                              << std::endl;
                }
                pending_au.clear();
                pending_dts_ms = -1;
                pending_pts_ms = -1;
                pending_eos    = false;
            };
            while (true)
            {
                const bool    stop_now   = stop_requested.load();
                const bool    input_done = encode_input_done.load();
                EncPacketView pkt_view;
                const int     get_packet_ret =
                    mpp_encoder_instance.EncoderGetPacket(&pkt_view);  // 拿到一个 packet
                if (get_packet_ret == mpp_enc_packet_result::kError)   // 出错分支
                {
                    if (stop_now && input_done)
                    {
                        break;
                    }
                    std::cerr << "Failed to get encoded packet from encoder" << std::endl;
                    fatal_error.store(true);
                    request_stop();
                    break;
                }
                if (get_packet_ret == mpp_enc_packet_result::kNoPacket)  // 当前无输出包
                {
                    // 如果是因为到达 EOS 导致无包，则先 flush 掉可能积压的
                    /// AU，再退出循环；否则继续等待包的产生。注意这里即使输入已经结束了，也可能存在编码器内部积压的
                    /// AU 没有及时输出，所以不能直接以输入结束作为退出条件。
                    if (mpp_encoder_instance.IsPacketEOS())
                    {
                        flush_pending_au();
                        request_stop();
                        break;
                    }
                    if (stop_now && input_done)
                    {
                        flush_pending_au();
                        break;
                    }
                    // 无包但未到
                    // EOS，说明编码器内部可能还在处理输入帧，继续等待输出包的产生；
                    // 如果一直等不到包了，则说明可能出现了问题，后续循环中会有超时统计输出。
                    std::this_thread::sleep_for(kEncodeNoPacketSleep);
                    continue;
                }
                if (get_packet_ret != mpp_enc_packet_result::kHasPacket)
                {
                    // 既不是出错也不是无包，理论上应该就是成功拿到包了，但这里加个兜底的日志输出以防万一。
                    std::cerr << "Unexpected encoder packet state: " << get_packet_ret << std::endl;
                    fatal_error.store(true);
                    request_stop();
                    break;
                }

                enc_pkt_total.fetch_add(1, std::memory_order_relaxed);  // 成功获取到包，计数器加一
                if (pkt_view.data && pkt_view.len > 0)                  // 确保数据有效
                {
                    if (pending_au.empty())  // 说明当前这个 packet 是一个新的 AU 的开始
                    {
                        pending_dts_ms = pkt_view.dts_ms;
                        pending_pts_ms = pkt_view.pts_ms;
                        pending_eos    = pkt_view.eos;
                    }
                    else
                    {
                        pending_eos = pending_eos || pkt_view.eos;
                    }
                    const uint8_t* packet_bytes_addr = static_cast<const uint8_t*>(pkt_view.data);
                    pending_au.insert(
                        pending_au.end(), packet_bytes_addr,
                        packet_bytes_addr +
                            pkt_view.len);  // 将当前 packet 的数据追加到 pending_au 中
                }
                const bool packet_is_partition = pkt_view.is_partition;
                const bool packet_is_eoi       = pkt_view.is_eoi;
                const bool packet_eos          = pkt_view.eos;
                if (mpp_encoder_instance.EncoderReleasePacket(&pkt_view) != 0)
                {
                    std::cerr << "Failed to release encoded packet" << std::endl;
                    fatal_error.store(true);
                    request_stop();
                    break;
                }
                const bool au_done = (!packet_is_partition) || packet_is_eoi || packet_eos;
                if (au_done)  // 完整的AU 拿到了，送给 ZLM 推流
                {
                    flush_pending_au();
                }

                const auto now_tp = std::chrono::steady_clock::now();
                if (now_tp - last_stat_tp >=
                    std::chrono::seconds(
                        5))  // 每 5 秒输出一次统计信息，帮助观察程序运行状态和性能指标
                {
                    last_stat_tp = now_tp;  // 更新上次输出统计的时间点
                    std::cout << "[ENC->ZLM] enc_pkt_total=" << enc_pkt_total.load()
                              << ", zlm_input_ok=" << zlm_input_ok.load()
                              << ", zlm_input_fail=" << zlm_input_fail.load()
                              << ", zlm_frame_out=" << zlm_publisher.GetOutputFrameCount()
                              << ", ts_fallback=" << zlm_publisher.GetTimestampFallbackCount()
                              << std::endl;
                }

                if (mpp_encoder_instance.IsPacketEOS())
                {
                    request_stop();
                    break;
                }
            }
        });

    while (!stop_requested.load())  // 主线程等待退出信号的触发
    {
        if (should_exit)
        {
            request_stop();
            break;
        }
        std::this_thread::sleep_for(kMainLoopSleep);
    }
    // 请求所有线程停止，并等待它们退出
    request_stop();
    if (capture_thread.joinable())
    {
        capture_thread.join();
    }
    if (decode_thread.joinable())
    {
        decode_thread.join();
    }
    if (rga_thread.joinable())
    {
        rga_thread.join();
    }
    if (mpp_encode_input_thread.joinable())
    {
        mpp_encode_input_thread.join();
    }
    if (mpp_encode_output_thread.joinable())
    {
        mpp_encode_output_thread.join();
    }
    if (mpp_output_recycle_thread.joinable())
    {
        mpp_output_recycle_thread.join();
    }
    drain_mpp_recycle_requests();  // 确保所有待回收的输出都被处理掉，避免资源泄漏

    {
        std::deque<const IO_FD_t*> pending_encode_inputs;
        {
            std::lock_guard<std::mutex> lock(mpp_encode_input_mutex);
            while (!mppEncodeInputQueue.empty())
            {
                pending_encode_inputs.push_back(mppEncodeInputQueue.front());
                mppEncodeInputQueue.pop_front();
            }
        }
        for (const IO_FD_t* output_desc : pending_encode_inputs)
        {
            if (rga_instance.QueueOutputToRecycle(output_desc) != 0)
            {
                std::cerr << "Failed to recycle pending encoder input descriptor, fd="
                          << ((output_desc) ? output_desc->fd : -1) << std::endl;
            }
        }
    }

    zlm_publisher.Close();

    // 退出阶段不再执行回队，交由 V4L2 反初始化流程（STREAMOFF + REQBUFS(0)）统一释放。
    {
        std::lock_guard<std::mutex> lock(ready_mutex);
        readyQueue.clear();
    }
    {
        std::lock_guard<std::mutex> lock(recycle_mutex);
        recycleQueue.clear();
    }
    {
        std::lock_guard<std::mutex> lock(rga_consume_mutex);
        rgaConsumeQueue.clear();
    }
    {
        std::lock_guard<std::mutex> lock(mpp_recycle_output_mutex);
        mppRecycleOutputQueue.clear();
    }
    {
        std::lock_guard<std::mutex> lock(mpp_encode_input_mutex);
        mppEncodeInputQueue.clear();
    }

    // 依赖析构函数完成资源的回收
    if (dropped_frames > 0)
    {
        std::cout << "Dropped old frames due to readyQueue backlog: " << dropped_frames
                  << std::endl;
    }
    std::cout << "Camera opened and closed successfully" << std::endl;
    return fatal_error.load() ? 1 : 0;
}
