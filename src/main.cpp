#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>

#include "rkmpp.h"
#include "v4l2Camera.h"

const std::string            device      = "/dev/video0";
static volatile sig_atomic_t should_exit = 0;  // 用于控制程序退出的标志

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

    V4L2_Camera camera;
    if (camera.camera_GlobalInit(device, 480) != 0)  // 全局初始化，设置曝光时间为 480ms
    {
        std::cerr << "Failed to initialize camera globally" << std::endl;
        return 1;
    }

    MppInstance mpp_instance;
    if (mpp_instance.MppInit() != 0)  // 初始化 MPP 实例
    {
        std::cerr << "Failed to initialize MPP instance" << std::endl;
        return 1;
    }
    if (mpp_instance.MppConfigWidthHeight(camera_params::kCaptureWidth,
                                          camera_params::kCaptureHeight) !=
        0)  // 配置 MPP 的宽高参数
    {
        std::cerr << "Failed to configure MPP width and height" << std::endl;
        return 1;
    }
    if (mpp_instance.MppAllocBuffer(camera.FrameDescArray,
                                    static_cast<size_t>(camera.get_buffer_count())) != 0)
    {
        std::cerr << "Failed to import V4L2 dma-buf into MPP input buffers" << std::endl;
        return 1;
    }

    std::mutex              ready_mutex;
    std::mutex              recycle_mutex;
    std::condition_variable ready_cv;
    std::deque<FrameDesc*>  readyQueue;
    std::deque<FrameDesc*>  recycleQueue;
    std::atomic_bool        stop_requested{false};
    std::atomic_bool        fatal_error{false};
    size_t                  dropped_frames      = 0;
    constexpr size_t        kMaxReadyQueueDepth = 2;  // 控制端到端延迟，超限时丢弃最旧帧

    auto request_stop = [&]()
    {
        stop_requested.store(true);
        ready_cv.notify_all();
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

                size_t frame_length      = camera_params::kMaxFrameSize;
                int    frame_capture_ret = camera.camera_read_frame(&frame_length,
                                                                  camera_params::kMaxFrameSize);
                if (frame_capture_ret == camera_read_result::kRetryable)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                if (frame_capture_ret == camera_read_result::kTimeout)
                {
                    std::cerr << "Camera dequeue timeout reached ("
                              << camera_params::kDequeueTimeoutMs
                              << " ms), stop capture thread" << std::endl;
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

                if (mpp_instance.MppDecode(frame_desc) != 0)
                {
                    // 解码失败直接跳过该帧，仅提交回收队列，保证采集链路继续前进。
                    std::cerr << "Failed to decode frame, index=" << frame_desc->index
                              << ", payload=" << frame_desc->payloadSize << std::endl;
                }
                else
                {
                    std::cout << "Frame captured and decoded successfully, length: "
                              << frame_desc->payloadSize << " bytes" << std::endl;
                }

                {
                    std::lock_guard<std::mutex> lock(recycle_mutex);
                    recycleQueue.push_back(frame_desc);
                }
            }

        });

    while (!stop_requested.load())
    {
        if (should_exit)
        {
            request_stop();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (capture_thread.joinable())
    {
        capture_thread.join();
    }
    request_stop();  // 保证解码线程在无任务时也能被唤醒退出
    if (decode_thread.joinable())
    {
        decode_thread.join();
    }

    // 退出阶段不再执行回队，交由 V4L2 反初始化流程（STREAMOFF + REQBUFS(0)）统一释放。
    {
        std::lock_guard<std::mutex> lock(ready_mutex);
        readyQueue.clear();
    }
    {
        std::lock_guard<std::mutex> lock(recycle_mutex);
        recycleQueue.clear();
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
