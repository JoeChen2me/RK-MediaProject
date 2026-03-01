#include "zlm_publisher.h"

#include <cstring>
#include <iostream>
#include <limits>
#include <string>

namespace
{
// WebRTC HTTP API 的固定路径。
constexpr const char kWebRtcApiPath[] = "/index/api/webrtc";
// WebRTC 子系统是否已成功启动（由 Init/Close 维护）。
bool gWebRtcServiceReady = false;
// 对外暴露给 WebRTC 客户端的固定 IP（用于 SDP/ICE）。
constexpr const char kWebRtcExternIp[] = "10.126.126.58";

// 所有 JSON 响应统一使用的 HTTP Header。
const char* kJsonResponseHeader[] = {"Content-Type", "application/json",
                                     "Access-Control-Allow-Origin", "*", nullptr};

std::string JsonEscape(const char* input)
{
    // 空指针直接返回空字符串，避免后续访问非法内存。
    if (!input)
    {
        return "";
    }

    // 预留空间减少扩容次数（原长度 + 少量转义冗余）。
    std::string escaped;
    escaped.reserve(std::strlen(input) + 32);
    // 逐字节扫描原字符串并执行 JSON 转义。
    for (const unsigned char ch : std::string(input))
    {
        switch (ch)
        {
            // 双引号必须转义，否则会破坏 JSON 字符串边界。
            case '"':
                escaped += "\\\"";
                break;
            // 反斜杠必须转义。
            case '\\':
                escaped += "\\\\";
                break;
            // 以下控制字符统一使用 JSON 标准转义序列。
            case '\b':
                escaped += "\\b";
                break;
            case '\f':
                escaped += "\\f";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                // 小于 0x20 的不可见控制字符，这里用 '?' 兜底。
                if (ch < 0x20)
                {
                    escaped += '?';
                }
                else
                {
                    // 普通可见字符直接追加。
                    escaped.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    return escaped;
}

void SendJsonResponse(const mk_http_response_invoker invoker, const std::string& body)
{
    // invoker 为空时无法回包，直接返回。
    if (!invoker)
    {
        return;
    }
    // 统一返回 200 + JSON body（业务错误由 JSON 内的 code 表示）。
    mk_http_response_invoker_do_string(invoker, 200, kJsonResponseHeader, body.c_str());
}

void SendWebRtcError(const mk_http_response_invoker invoker, const char* err_msg)
{
    // 组装统一错误 JSON：{"code":-1,"msg":"..."}。
    std::string body = "{\"code\":-1,\"msg\":\"";
    // 错误信息做 JSON 转义，避免破坏响应格式。
    body += JsonEscape(err_msg ? err_msg : "invalid request");
    body += "\"}";
    // 发送错误响应。
    SendJsonResponse(invoker, body);
}

void API_CALL OnWebRtcAnswerSdp(void* user_data, const char* answer, const char* err)
{
    // user_data 里放的是克隆后的 HTTP invoker。
    mk_http_response_invoker invoker = static_cast<mk_http_response_invoker>(user_data);
    if (!invoker)
    {
        return;
    }

    // 成功拿到 answer 时，返回标准 WebRTC answer JSON。
    if (answer)
    {
        std::string body = "{\"code\":0,\"type\":\"answer\",\"sdp\":\"";
        body += JsonEscape(answer);
        body += "\"}";
        SendJsonResponse(invoker, body);
    }
    else
    {
        // 失败时返回统一错误 JSON。
        SendWebRtcError(invoker, err ? err : "failed to generate webrtc answer");
    }

    // 异步回调结束后释放克隆对象，防止泄漏。
    mk_http_response_invoker_clone_release(invoker);
}

void API_CALL OnMkHttpRequest(const mk_parser parser, const mk_http_response_invoker invoker,
                              int* consumed, const mk_sock_info /*sender*/)
{
    // consumed 必须可写，用于告诉 ZLM 当前请求是否已被消费。
    if (!consumed)
    {
        return;
    }
    // 默认不消费，只有命中目标 API 后才置 1。
    *consumed = 0;
    // 解析器或回包器为空时无法处理。
    if (!parser || !invoker)
    {
        return;
    }

    // 仅处理 /index/api/webrtc，其他请求交给默认处理链。
    const char* url = mk_parser_get_url(parser);
    if (!url || std::strcmp(url, kWebRtcApiPath) != 0)
    {
        return;
    }
    // 命中后标记已消费，防止被其他 handler 再处理。
    *consumed = 1;

    // RTC 未就绪时直接返回错误，避免继续走 SDP 流程。
    if (!gWebRtcServiceReady)
    {
        SendWebRtcError(invoker,
                        "webrtc is unavailable: rtc server not started or ENABLE_WEBRTC disabled");
        return;
    }

    // 异步流程需要延迟回包，因此先克隆 invoker。
    mk_http_response_invoker invoker_clone = mk_http_response_invoker_clone(invoker);
    if (!invoker_clone)
    {
        SendWebRtcError(invoker, "failed to clone http invoker");
        return;
    }

    // 当前仅支持播放协商：type=play。
    const char* type = mk_parser_get_url_param(parser, "type");
    if (!type || std::strcmp(type, "play") != 0)
    {
        SendWebRtcError(invoker_clone, "only type=play is supported");
        mk_http_response_invoker_clone_release(invoker_clone);
        return;
    }

    // app 不能为空（对应流路径中的 app 段）。
    const char* app = mk_parser_get_url_param(parser, "app");
    if (!app || app[0] == '\0')
    {
        SendWebRtcError(invoker_clone, "missing app query parameter");
        mk_http_response_invoker_clone_release(invoker_clone);
        return;
    }

    // stream 不能为空（对应流路径中的 stream 段）。
    const char* stream = mk_parser_get_url_param(parser, "stream");
    if (!stream || stream[0] == '\0')
    {
        SendWebRtcError(invoker_clone, "missing stream query parameter");
        mk_http_response_invoker_clone_release(invoker_clone);
        return;
    }

    // HTTP body 中必须带 offer SDP。
    const char* offer = mk_parser_get_content(parser, nullptr);
    if (!offer || offer[0] == '\0')
    {
        SendWebRtcError(invoker_clone, "missing webrtc offer in http body");
        mk_http_response_invoker_clone_release(invoker_clone);
        return;
    }

    // 组装 rtc:// URL（用于 ZLM 内部定位目标流）。
    std::string rtc_url = "rtc://";
    // 强制使用固定对外 IP，确保候选地址可达。
    rtc_url += kWebRtcExternIp;
    rtc_url += "/";
    rtc_url += app;
    rtc_url += "/";
    rtc_url += stream;

    // 原请求上的 query 参数透传到 rtc_url，便于保留扩展字段。
    const char* params = mk_parser_get_url_params(parser);
    if (params && params[0] != '\0')
    {
        rtc_url += "?";
        rtc_url += params;
    }

    // 发起异步 SDP 交换，结果通过 OnWebRtcAnswerSdp 回调返回。
    mk_webrtc_get_answer_sdp(invoker_clone, OnWebRtcAnswerSdp, type, offer, rtc_url.c_str());
}
}  // namespace

// 默认构造：成员使用类内默认值。
ZlmPublisher::ZlmPublisher() {}

// 析构时主动释放资源，确保服务器/媒体对象关闭。
ZlmPublisher::~ZlmPublisher() { Close(); }

int ZlmPublisher::Init(const ZlmPublishConfig& cfg)
{
    // 已初始化时直接成功返回，避免重复启动服务。
    if (Initialized_)
    {
        return 0;
    }
    // 保存发布配置，供后续 URL 与服务端口使用。
    PublishConfig_ = cfg;

    // 组装 ZLM 环境初始化参数。
    mk_config config = {};
    // 不从 ini 文件加载，全部使用代码设置。
    config.ini         = nullptr;
    config.ini_is_path = 0;
    // log_level=0 表示按 ZLM 默认日志级别。
    config.log_level = 0;
    // 日志输出到控制台。
    config.log_mask      = LOG_CONSOLE;
    config.log_file_path = nullptr;
    config.log_file_days = 0;
    // 不加载 SSL 证书文件。
    config.ssl         = nullptr;
    config.ssl_is_path = 1;
    config.ssl_pwd     = nullptr;
    // 线程数为 0，交给 ZLM 自动决定。
    config.thread_num = 0;
    // 初始化 ZLM 全局环境（只应调用一次）。
    mk_env_init(&config);

    // 启动 HTTP 服务（用于 WebRTC API 等），第二个参数 0 表示不启用 SSL。
    HttpPort_ = mk_http_server_start(PublishConfig_.http_port, 0);  // HTTP 服务器不启用 SSL
    if (HttpPort_ == 0)
    {
        // 启动失败时清理已启动资源并返回错误。
        Close();
        return -1;
    }
    // 启动 RTSP 服务（用于 RTSP 拉流）。
    RtspPort_ = mk_rtsp_server_start(PublishConfig_.rtsp_port, 0);  // RTSP 服务器不启用 SSL
    if (RtspPort_ == 0)
    {
        Close();
        return -1;
    }

    // 默认使用配置里的 RTC 端口。
    uint16_t rtc_target_port = PublishConfig_.rtc_port;
    // 避免 RTC 与 HTTP 端口冲突（会导致监听失败/冲突）。
    if (rtc_target_port == HttpPort_)
    {
        // 简单策略：可加 1 就加 1，否则回落到 8001。
        rtc_target_port = (rtc_target_port < 65535u) ? static_cast<uint16_t>(rtc_target_port + 1u)
                                                     : static_cast<uint16_t>(8001u);
        std::cerr << "[ZLM] rtc_port conflicts with http_port(" << HttpPort_
                  << "), switch rtc_port to " << rtc_target_port << std::endl;
    }

    // 读取全局 ini 对象，用于设置 rtc 配置项。
    mk_ini global_ini = mk_ini_default();
    if (global_ini)
    {
        // 设置对外可见 IP，影响 ICE candidate 地址生成。
        mk_ini_set_option(global_ini, "rtc.externIP", kWebRtcExternIp);
        // 设置 RTC UDP 端口。
        mk_ini_set_option_int(global_ini, "rtc.port", rtc_target_port);
        // 设置 RTC TCP 端口（UDP 不通时可回退）。
        mk_ini_set_option_int(global_ini, "rtc.tcpPort", rtc_target_port);
    }

    // 启动 RTC 服务并记录实际端口（可能与期望端口不同）。
    RtcPort_ = mk_rtc_server_start(rtc_target_port);
    // 标记 WebRTC 服务可用状态。
    gWebRtcServiceReady = (RtcPort_ != 0);
    if (!gWebRtcServiceReady)
    {
        std::cerr << "[ZLM] RTC server failed to start, WebRTC API will return error JSON"
                  << std::endl;
    }
    else if (global_ini && RtcPort_ != rtc_target_port)
    {
        // Keep SDP candidate port aligned with actual listening port.
        // 若端口被系统调整，反向写回 ini，确保 SDP 中端口与监听端口一致。
        mk_ini_set_option_int(global_ini, "rtc.port", RtcPort_);
        mk_ini_set_option_int(global_ini, "rtc.tcpPort", RtcPort_);
    }

    // 注册事件回调：当前主要用到 HTTP 请求回调。
    mk_events events          = {};
    events.on_mk_http_request = OnMkHttpRequest;
    // 生效事件监听配置。
    mk_events_listen(&events);

    // 创建媒体选项对象，用于控制协议开关。
    mk_ini media_option = mk_ini_create();
    if (!media_option)
    {
        Close();
        return -1;
    }

    // 只开启 RTSP，其他协议全部关闭以简化场景并减少开销。
    mk_ini_set_option_int(media_option, "enable_rtsp", 1);
    mk_ini_set_option_int(media_option, "enable_rtmp", 0);
    mk_ini_set_option_int(media_option, "enable_hls", 0);
    mk_ini_set_option_int(media_option, "enable_hls_fmp4", 0);
    mk_ini_set_option_int(media_option, "enable_ts", 0);
    mk_ini_set_option_int(media_option, "enable_fmp4", 0);
    mk_ini_set_option_int(media_option, "enable_mp4", 0);
    mk_ini_set_option_int(media_option, "enable_audio", 0);

    // 创建媒体源对象（vhost/app/stream）。
    Media_ = mk_media_create2(PublishConfig_.vhost.c_str(), PublishConfig_.app.c_str(),
                              PublishConfig_.stream.c_str(), 0.0f, media_option);
    // 选项对象用完立即释放。
    mk_ini_release(media_option);

    // 媒体源创建失败则整体初始化失败。
    if (!Media_)
    {
        Close();
        return -1;
    }

    // 创建 H264 视频轨道。
    codec_args track_args = {};
    mk_track   track      = mk_track_create(MKCodecH264, &track_args);
    if (!track)
    {
        Close();
        return -1;
    }
    // 把轨道挂到媒体源上。
    mk_media_init_track(Media_, track);
    // 标记轨道初始化完成，允许外部订阅。
    mk_media_init_complete(Media_);
    // 释放本地引用（媒体源内部已持有）。
    mk_track_unref(track);

    // 到这里说明初始化完全成功。
    Initialized_ = true;
    return 0;
}

void ZlmPublisher::SetExpectedFps(uint32_t fps)
{
    // 防御：传 0 时回退到 30fps。
    if (fps == 0)
    {
        fps = 30;
    }
    // 计算单帧时长（微秒）。
    FrameIntervalUs_ = 1000000 / static_cast<uint64_t>(fps);
    // 再做一次保护，避免后续出现 0 步长。
    if (FrameIntervalUs_ == 0)
    {
        FrameIntervalUs_ = 1;
    }
}

int ZlmPublisher::InputPacketChunk(const EncPacketView& pkt)
{
    // 未初始化或媒体源无效时，拒绝输入。
    if (!Initialized_ || !Media_)
    {
        return -1;
    }
    // 包数据为空或长度为 0，属于无效输入。
    if (!pkt.data || pkt.len == 0)
    {
        return -1;
    }
    // mk_media_input_h264 需要 int 长度，这里防止 size_t 转 int 溢出。
    if (pkt.len > static_cast<size_t>(std::numeric_limits<int>::max()))
    {
        return -1;
    }

    // 把泛型指针视为字节流。
    const uint8_t* bytes = static_cast<const uint8_t*>(pkt.data);
    // 只接受 AnnexB（带起始码）格式的 H264 包。
    if (!IsAnnexBStartCode(bytes, pkt.len))
    {
        return -1;
    }

    // 归一化时间戳到单调递增毫秒值，供 ZLM 输入接口使用。
    uint64_t dts_ms = 0;
    uint64_t pts_ms = 0;
    if (!NormalizeTimestamp(pkt, &dts_ms, &pts_ms))
    {
        return -1;
    }

    // 把一个大包拆成多个 NALU（每个 NALU 仍保留起始码）。
    std::vector<NaluRange> nalus;
    if (!SplitAnnexBNalus(bytes, pkt.len, &nalus) || nalus.empty())
    {
        return -1;
    }
    // 逐个 NALU 输入到 ZLM。
    for (const auto& nalu : nalus)
    {
        // NALU 基本合法性检查。
        if (!nalu.data || nalu.len == 0 ||
            nalu.len > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            return -1;
        }
        // 输入 H264 数据到媒体源。
        const int ret =
            mk_media_input_h264(Media_, nalu.data, static_cast<int>(nalu.len), dts_ms, pts_ms);
        // ZLM 返回 0 代表失败。
        if (!ret)
        {
            return -1;
        }
    }

    // 统计成功输出帧数（按输入 chunk 统计）。
    OutputFrameCount_++;
    return 0;
}

std::string ZlmPublisher::GetRtspUrl() const
{
    // 服务未启动时返回空串。
    if (RtspPort_ == 0)
    {
        return "";
    }

    // 组装展示用 RTSP URL（设备 IP 留给用户替换）。
    std::string url = "rtsp://<设备IP>:";
    url += std::to_string(RtspPort_);
    url += "/";
    url += PublishConfig_.app;
    url += "/";
    url += PublishConfig_.stream;
    return url;
}

std::string ZlmPublisher::GetWebRtcApiUrl() const
{
    // 服务未启动时返回空串。
    if (HttpPort_ == 0)
    {
        return "";
    }

    // 组装展示用 WebRTC API URL（设备 IP 留给用户替换）。
    std::string url = "http://<设备IP>:";
    url += std::to_string(HttpPort_);
    url += kWebRtcApiPath;
    url += "?app=";
    url += PublishConfig_.app;
    url += "&stream=";
    url += PublishConfig_.stream;
    url += "&type=play";
    return url;
}

void ZlmPublisher::Close()
{
    // 取消事件监听，避免回调继续触发。
    mk_events_listen(nullptr);

    // 释放媒体源对象。
    if (Media_)
    {
        mk_media_release(Media_);
        Media_ = nullptr;
    }

    // 关闭全部 ZLM 服务（HTTP/RTSP/RTC 等）。
    mk_stop_all_server();
    // 同步更新 WebRTC 可用标记。
    gWebRtcServiceReady = false;

    // 复位时间戳状态与端口状态，便于后续重新 Init。
    LastDtsUs_        = 0;
    LastPtsUs_        = 0;
    LastDtsMs_        = 0;
    LastPtsMs_        = 0;
    RtspPort_         = 0;
    HttpPort_         = 0;
    RtcPort_          = 0;
    HasLastTimestamp_ = false;
    Initialized_      = false;
}

bool ZlmPublisher::NormalizeTimestamp(const EncPacketView& pkt, uint64_t* out_dts_ms,
                                      uint64_t* out_pts_ms)
{
    // 输出参数必须有效。
    if (!out_dts_ms || !out_pts_ms)
    {
        return false;
    }

    // 记录本次是否使用了回退逻辑（用于统计）。
    bool used_fallback = false;
    // 读取输入包中的微秒时间戳。
    int64_t dts_us = pkt.dts_us;
    int64_t pts_us = pkt.pts_us;

    // 若 dts 缺失但 pts 可用，则用 pts 补 dts。
    if (dts_us < 0 && pts_us >= 0)
    {
        dts_us        = pts_us;
        used_fallback = true;
    }
    // 若 pts 缺失但 dts 可用，则用 dts 补 pts。
    if (pts_us < 0 && dts_us >= 0)
    {
        pts_us        = dts_us;
        used_fallback = true;
    }

    // 若两者仍有缺失，则按期望帧间隔进行时间戳补点。
    if (dts_us < 0 || pts_us < 0)
    {
        // 步长使用设置的帧间隔，至少为 1us。
        const uint64_t step_us = (FrameIntervalUs_ > 0 ? FrameIntervalUs_ : 1);
        // 第一帧从 0 开始。
        if (!HasLastTimestamp_)
        {
            dts_us = 0;
            pts_us = 0;
        }
        else
        {
            // 后续帧在上一帧基础上递增。
            dts_us = static_cast<int64_t>(LastDtsUs_ + step_us);
            pts_us = dts_us;
        }
        used_fallback = true;
    }

    // 保证 dts_us 单调递增（至少比上一帧大 1us）。
    if (HasLastTimestamp_ && static_cast<uint64_t>(dts_us) <= LastDtsUs_)
    {
        dts_us        = static_cast<int64_t>(LastDtsUs_ + 1);
        used_fallback = true;
    }
    // 保证 pts_us 不小于 dts_us。
    if (pts_us < dts_us)
    {
        pts_us        = dts_us;
        used_fallback = true;
    }

    // 微秒转毫秒，供 ZLM 接口使用。
    uint64_t dts_ms = static_cast<uint64_t>(dts_us / 1000);
    uint64_t pts_ms = static_cast<uint64_t>(pts_us / 1000);
    // 毫秒层面继续保证单调（避免除法截断导致重复）。
    if (HasLastTimestamp_ && dts_ms <= LastDtsMs_)
    {
        dts_ms        = LastDtsMs_ + 1;
        used_fallback = true;
    }
    // 保证 pts_ms 不小于 dts_ms。
    if (pts_ms < dts_ms)
    {
        pts_ms        = dts_ms;
        used_fallback = true;
    }

    // 回写最新时间戳状态。
    LastDtsUs_        = static_cast<uint64_t>(dts_us);
    LastPtsUs_        = static_cast<uint64_t>(pts_us);
    LastDtsMs_        = dts_ms;
    LastPtsMs_        = pts_ms;
    HasLastTimestamp_ = true;
    // 记录回退次数，便于日志诊断时间戳质量。
    if (used_fallback)
    {
        TimestampFallbackCount_++;
    }

    // 输出归一化结果。
    *out_dts_ms = LastDtsMs_;
    *out_pts_ms = LastPtsMs_;
    return true;
}

bool ZlmPublisher::IsAnnexBStartCode(const uint8_t* data, size_t len) const
{
    // 至少要有 3 字节才可能是 00 00 01。
    if (!data || len < 3)
    {
        return false;
    }

    // 先判断 4 字节起始码 00 00 00 01。
    if (len >= 4 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x01)
    {
        return true;
    }

    // 再判断 3 字节起始码 00 00 01。
    if (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01)
    {
        return true;
    }

    // 不是 AnnexB 起始码。
    return false;
}

bool ZlmPublisher::FindAnnexBPrefix(const uint8_t* data, size_t len, size_t pos,
                                    size_t* prefix_len) const
{
    // 基础参数校验：数据、输出指针、扫描位置。
    if (!data || !prefix_len || pos >= len)
    {
        return false;
    }
    // 命中 4 字节前缀则返回长度 4。
    if (pos + 4 <= len && data[pos] == 0x00 && data[pos + 1] == 0x00 && data[pos + 2] == 0x00 &&
        data[pos + 3] == 0x01)
    {
        *prefix_len = 4;
        return true;
    }
    // 命中 3 字节前缀则返回长度 3。
    if (pos + 3 <= len && data[pos] == 0x00 && data[pos + 1] == 0x00 && data[pos + 2] == 0x01)
    {
        *prefix_len = 3;
        return true;
    }
    // 未命中。
    return false;
}

bool ZlmPublisher::SplitAnnexBNalus(const uint8_t* data, size_t len,
                                    std::vector<NaluRange>* out_nalus) const
{
    // 基础参数校验。
    if (!data || !out_nalus || len < 3)
    {
        return false;
    }
    // 先清空输出，避免残留旧数据。
    out_nalus->clear();

    // 收集所有起始码位置。
    std::vector<size_t> starts;
    // 预留常见容量，减少扩容。
    starts.reserve(16);
    // 扫描整个码流，查找每个 AnnexB 前缀。
    for (size_t pos = 0; pos + 3 <= len;)
    {
        size_t prefix_len = 0;
        if (FindAnnexBPrefix(data, len, pos, &prefix_len))
        {
            // 记录起始位置并跳过前缀长度，继续往后找。
            starts.push_back(pos);
            pos += prefix_len;
        }
        else
        {
            // 未命中就向后移动 1 字节继续匹配。
            ++pos;
        }
    }

    // 必须至少有一个 NALU，且第一个前缀应位于开头。
    if (starts.empty() || starts[0] != 0)
    {
        return false;
    }

    // 用相邻起始位置切分每个 NALU 的 [start, end) 区间。
    for (size_t i = 0; i < starts.size(); ++i)
    {
        const size_t start = starts[i];
        const size_t end   = (i + 1 < starts.size()) ? starts[i + 1] : len;
        // 异常区间直接跳过。
        if (end <= start)
        {
            continue;
        }
        // 记录 NALU 指针和长度（零拷贝引用原缓冲区）。
        NaluRange nalu;
        nalu.data = data + start;
        nalu.len  = end - start;
        out_nalus->push_back(nalu);
    }

    // 至少切出一个 NALU 才算成功。
    return !out_nalus->empty();
}
