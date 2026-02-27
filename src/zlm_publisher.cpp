#include "zlm_publisher.h"

#include <cstring>
#include <iostream>
#include <limits>
#include <string>

namespace
{
constexpr const char kWebRtcApiPath[]    = "/index/api/webrtc";
bool                 gWebRtcServiceReady = false;

const char* kJsonResponseHeader[] = {"Content-Type", "application/json",
                                     "Access-Control-Allow-Origin", "*", nullptr};

std::string JsonEscape(const char* input)
{
    if (!input)
    {
        return "";
    }

    std::string escaped;
    escaped.reserve(std::strlen(input) + 32);
    for (const unsigned char ch : std::string(input))
    {
        switch (ch)
        {
            case '"':
                escaped += "\\\"";
                break;
            case '\\':
                escaped += "\\\\";
                break;
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
                if (ch < 0x20)
                {
                    escaped += '?';
                }
                else
                {
                    escaped.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    return escaped;
}

void SendJsonResponse(const mk_http_response_invoker invoker, const std::string& body)
{
    if (!invoker)
    {
        return;
    }
    mk_http_response_invoker_do_string(invoker, 200, kJsonResponseHeader, body.c_str());
}

void SendWebRtcError(const mk_http_response_invoker invoker, const char* err_msg)
{
    std::string body = "{\"code\":-1,\"msg\":\"";
    body += JsonEscape(err_msg ? err_msg : "invalid request");
    body += "\"}";
    SendJsonResponse(invoker, body);
}

void API_CALL OnWebRtcAnswerSdp(void* user_data, const char* answer, const char* err)
{
    mk_http_response_invoker invoker = static_cast<mk_http_response_invoker>(user_data);
    if (!invoker)
    {
        return;
    }

    if (answer)
    {
        std::string body = "{\"code\":0,\"type\":\"answer\",\"sdp\":\"";
        body += JsonEscape(answer);
        body += "\"}";
        SendJsonResponse(invoker, body);
    }
    else
    {
        SendWebRtcError(invoker, err ? err : "failed to generate webrtc answer");
    }

    mk_http_response_invoker_clone_release(invoker);
}

void API_CALL OnMkHttpRequest(const mk_parser parser, const mk_http_response_invoker invoker,
                              int* consumed, const mk_sock_info /*sender*/)
{
    if (!consumed)
    {
        return;
    }
    *consumed = 0;
    if (!parser || !invoker)
    {
        return;
    }

    const char* url = mk_parser_get_url(parser);
    if (!url || std::strcmp(url, kWebRtcApiPath) != 0)
    {
        return;
    }
    *consumed = 1;

    if (!gWebRtcServiceReady)
    {
        SendWebRtcError(invoker,
                        "webrtc is unavailable: rtc server not started or ENABLE_WEBRTC disabled");
        return;
    }

    mk_http_response_invoker invoker_clone = mk_http_response_invoker_clone(invoker);
    if (!invoker_clone)
    {
        SendWebRtcError(invoker, "failed to clone http invoker");
        return;
    }

    const char* type = mk_parser_get_url_param(parser, "type");
    if (!type || std::strcmp(type, "play") != 0)
    {
        SendWebRtcError(invoker_clone, "only type=play is supported");
        mk_http_response_invoker_clone_release(invoker_clone);
        return;
    }

    const char* app = mk_parser_get_url_param(parser, "app");
    if (!app || app[0] == '\0')
    {
        SendWebRtcError(invoker_clone, "missing app query parameter");
        mk_http_response_invoker_clone_release(invoker_clone);
        return;
    }

    const char* stream = mk_parser_get_url_param(parser, "stream");
    if (!stream || stream[0] == '\0')
    {
        SendWebRtcError(invoker_clone, "missing stream query parameter");
        mk_http_response_invoker_clone_release(invoker_clone);
        return;
    }

    const char* offer = mk_parser_get_content(parser, nullptr);
    if (!offer || offer[0] == '\0')
    {
        SendWebRtcError(invoker_clone, "missing webrtc offer in http body");
        mk_http_response_invoker_clone_release(invoker_clone);
        return;
    }

    const char* host = mk_parser_get_header(parser, "Host");
    if (!host || host[0] == '\0')
    {
        host = "127.0.0.1";
    }

    std::string rtc_url = "rtc://";
    rtc_url += host;
    rtc_url += "/";
    rtc_url += app;
    rtc_url += "/";
    rtc_url += stream;

    const char* params = mk_parser_get_url_params(parser);
    if (params && params[0] != '\0')
    {
        rtc_url += "?";
        rtc_url += params;
    }

    mk_webrtc_get_answer_sdp(invoker_clone, OnWebRtcAnswerSdp, type, offer, rtc_url.c_str());
}
}  // namespace

ZlmPublisher::ZlmPublisher() {}

ZlmPublisher::~ZlmPublisher() { Close(); }

int ZlmPublisher::Init(const ZlmPublishConfig& cfg)
{
    if (Initialized_)
    {
        return 0;
    }
    PublishConfig_ = cfg;

    mk_config config     = {};
    config.ini           = nullptr;
    config.ini_is_path   = 0;
    config.log_level     = 0;
    config.log_mask      = LOG_CONSOLE;
    config.log_file_path = nullptr;
    config.log_file_days = 0;
    config.ssl           = nullptr;
    config.ssl_is_path   = 1;
    config.ssl_pwd       = nullptr;
    config.thread_num    = 0;
    mk_env_init(&config);

    HttpPort_ = mk_http_server_start(PublishConfig_.http_port, 0);
    if (HttpPort_ == 0)
    {
        Close();
        return -1;
    }
    RtspPort_ = mk_rtsp_server_start(PublishConfig_.rtsp_port, 0);
    if (RtspPort_ == 0)
    {
        Close();
        return -1;
    }

    uint16_t rtc_target_port = PublishConfig_.rtc_port;
    if (rtc_target_port == HttpPort_)
    {
        rtc_target_port = (rtc_target_port < 65535u) ? static_cast<uint16_t>(rtc_target_port + 1u)
                                                     : static_cast<uint16_t>(8001u);
        std::cerr << "[ZLM] rtc_port conflicts with http_port(" << HttpPort_
                  << "), switch rtc_port to " << rtc_target_port << std::endl;
    }

    mk_ini global_ini = mk_ini_default();
    if (global_ini)
    {
        mk_ini_set_option_int(global_ini, "rtc.port", rtc_target_port);
        mk_ini_set_option_int(global_ini, "rtc.tcpPort", rtc_target_port);
    }

    RtcPort_            = mk_rtc_server_start(rtc_target_port);
    gWebRtcServiceReady = (RtcPort_ != 0);
    if (!gWebRtcServiceReady)
    {
        std::cerr << "[ZLM] RTC server failed to start, WebRTC API will return error JSON"
                  << std::endl;
    }
    else if (global_ini && RtcPort_ != rtc_target_port)
    {
        // Keep SDP candidate port aligned with actual listening port.
        mk_ini_set_option_int(global_ini, "rtc.port", RtcPort_);
        mk_ini_set_option_int(global_ini, "rtc.tcpPort", RtcPort_);
    }

    mk_events events          = {};
    events.on_mk_http_request = OnMkHttpRequest;
    mk_events_listen(&events);

    mk_ini media_option = mk_ini_create();
    if (!media_option)
    {
        Close();
        return -1;
    }

    mk_ini_set_option_int(media_option, "enable_rtsp", 1);
    mk_ini_set_option_int(media_option, "enable_rtmp", 0);
    mk_ini_set_option_int(media_option, "enable_hls", 0);
    mk_ini_set_option_int(media_option, "enable_hls_fmp4", 0);
    mk_ini_set_option_int(media_option, "enable_ts", 0);
    mk_ini_set_option_int(media_option, "enable_fmp4", 0);
    mk_ini_set_option_int(media_option, "enable_mp4", 0);
    mk_ini_set_option_int(media_option, "enable_audio", 0);

    Media_ = mk_media_create2(PublishConfig_.vhost.c_str(), PublishConfig_.app.c_str(),
                              PublishConfig_.stream.c_str(), 0.0f, media_option);
    mk_ini_release(media_option);

    if (!Media_)
    {
        Close();
        return -1;
    }

    codec_args track_args = {};
    mk_track   track      = mk_track_create(MKCodecH264, &track_args);
    if (!track)
    {
        Close();
        return -1;
    }
    mk_media_init_track(Media_, track);
    mk_media_init_complete(Media_);
    mk_track_unref(track);

    Initialized_ = true;
    return 0;
}

void ZlmPublisher::SetExpectedFps(uint32_t fps)
{
    if (fps == 0)
    {
        fps = 30;
    }
    FrameIntervalUs_ = 1000000 / static_cast<uint64_t>(fps);
    if (FrameIntervalUs_ == 0)
    {
        FrameIntervalUs_ = 1;
    }
}

int ZlmPublisher::InputPacketChunk(const EncPacketView& pkt)
{
    if (!Initialized_ || !Media_)
    {
        return -1;
    }
    if (!pkt.data || pkt.len == 0)
    {
        return -1;
    }
    if (pkt.len > static_cast<size_t>(std::numeric_limits<int>::max()))
    {
        return -1;
    }

    const uint8_t* bytes = static_cast<const uint8_t*>(pkt.data);
    if (!IsAnnexBStartCode(bytes, pkt.len))
    {
        return -1;
    }

    uint64_t dts_ms = 0;
    uint64_t pts_ms = 0;
    if (!NormalizeTimestamp(pkt, &dts_ms, &pts_ms))
    {
        return -1;
    }

    std::vector<NaluRange> nalus;
    if (!SplitAnnexBNalus(bytes, pkt.len, &nalus) || nalus.empty())
    {
        return -1;
    }
    for (const auto& nalu : nalus)
    {
        if (!nalu.data || nalu.len == 0 ||
            nalu.len > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            return -1;
        }
        const int ret =
            mk_media_input_h264(Media_, nalu.data, static_cast<int>(nalu.len), dts_ms, pts_ms);
        if (!ret)
        {
            return -1;
        }
    }

    OutputFrameCount_++;
    return 0;
}

std::string ZlmPublisher::GetRtspUrl() const
{
    if (RtspPort_ == 0)
    {
        return "";
    }

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
    if (HttpPort_ == 0)
    {
        return "";
    }

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
    mk_events_listen(nullptr);

    if (Media_)
    {
        mk_media_release(Media_);
        Media_ = nullptr;
    }

    mk_stop_all_server();
    gWebRtcServiceReady = false;

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
    if (!out_dts_ms || !out_pts_ms)
    {
        return false;
    }

    bool    used_fallback = false;
    int64_t dts_us        = pkt.dts_us;
    int64_t pts_us        = pkt.pts_us;

    if (dts_us < 0 && pts_us >= 0)
    {
        dts_us        = pts_us;
        used_fallback = true;
    }
    if (pts_us < 0 && dts_us >= 0)
    {
        pts_us        = dts_us;
        used_fallback = true;
    }

    if (dts_us < 0 || pts_us < 0)
    {
        const uint64_t step_us = (FrameIntervalUs_ > 0 ? FrameIntervalUs_ : 1);
        if (!HasLastTimestamp_)
        {
            dts_us = 0;
            pts_us = 0;
        }
        else
        {
            dts_us = static_cast<int64_t>(LastDtsUs_ + step_us);
            pts_us = dts_us;
        }
        used_fallback = true;
    }

    if (HasLastTimestamp_ && static_cast<uint64_t>(dts_us) <= LastDtsUs_)
    {
        dts_us        = static_cast<int64_t>(LastDtsUs_ + 1);
        used_fallback = true;
    }
    if (pts_us < dts_us)
    {
        pts_us        = dts_us;
        used_fallback = true;
    }

    uint64_t dts_ms = static_cast<uint64_t>(dts_us / 1000);
    uint64_t pts_ms = static_cast<uint64_t>(pts_us / 1000);
    if (HasLastTimestamp_ && dts_ms <= LastDtsMs_)
    {
        dts_ms        = LastDtsMs_ + 1;
        used_fallback = true;
    }
    if (pts_ms < dts_ms)
    {
        pts_ms        = dts_ms;
        used_fallback = true;
    }

    LastDtsUs_        = static_cast<uint64_t>(dts_us);
    LastPtsUs_        = static_cast<uint64_t>(pts_us);
    LastDtsMs_        = dts_ms;
    LastPtsMs_        = pts_ms;
    HasLastTimestamp_ = true;
    if (used_fallback)
    {
        TimestampFallbackCount_++;
    }

    *out_dts_ms = LastDtsMs_;
    *out_pts_ms = LastPtsMs_;
    return true;
}

bool ZlmPublisher::IsAnnexBStartCode(const uint8_t* data, size_t len) const
{
    if (!data || len < 3)
    {
        return false;
    }

    if (len >= 4 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x01)
    {
        return true;
    }

    if (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01)
    {
        return true;
    }

    return false;
}

bool ZlmPublisher::FindAnnexBPrefix(const uint8_t* data, size_t len, size_t pos,
                                    size_t* prefix_len) const
{
    if (!data || !prefix_len || pos >= len)
    {
        return false;
    }
    if (pos + 4 <= len && data[pos] == 0x00 && data[pos + 1] == 0x00 && data[pos + 2] == 0x00 &&
        data[pos + 3] == 0x01)
    {
        *prefix_len = 4;
        return true;
    }
    if (pos + 3 <= len && data[pos] == 0x00 && data[pos + 1] == 0x00 && data[pos + 2] == 0x01)
    {
        *prefix_len = 3;
        return true;
    }
    return false;
}

bool ZlmPublisher::SplitAnnexBNalus(const uint8_t* data, size_t len,
                                    std::vector<NaluRange>* out_nalus) const
{
    if (!data || !out_nalus || len < 3)
    {
        return false;
    }
    out_nalus->clear();

    std::vector<size_t> starts;
    starts.reserve(16);
    for (size_t pos = 0; pos + 3 <= len;)
    {
        size_t prefix_len = 0;
        if (FindAnnexBPrefix(data, len, pos, &prefix_len))
        {
            starts.push_back(pos);
            pos += prefix_len;
        }
        else
        {
            ++pos;
        }
    }

    if (starts.empty() || starts[0] != 0)
    {
        return false;
    }

    for (size_t i = 0; i < starts.size(); ++i)
    {
        const size_t start = starts[i];
        const size_t end   = (i + 1 < starts.size()) ? starts[i + 1] : len;
        if (end <= start)
        {
            continue;
        }
        NaluRange nalu;
        nalu.data = data + start;
        nalu.len  = end - start;
        out_nalus->push_back(nalu);
    }

    return !out_nalus->empty();
}
