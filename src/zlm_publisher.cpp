#include "zlm_publisher.h"

#include <limits>

namespace
{
constexpr uint16_t kRtspPort = 8554;
}  // namespace

ZlmPublisher::ZlmPublisher() {}

ZlmPublisher::~ZlmPublisher() { Close(); }

int ZlmPublisher::Init()
{
    if (Initialized_)
    {
        return 0;
    }

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

    if (mk_rtsp_server_start(kRtspPort, 0) == 0)
    {
        return -1;
    }

    Media_ = mk_media_create("__defaultVhost__", "live", "camera", 0, 0, 0);
    if (!Media_)
    {
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

void ZlmPublisher::Close()
{
    if (Media_)
    {
        mk_media_release(Media_);
        Media_ = nullptr;
    }

    mk_stop_all_server();

    LastDtsMs_        = 0;
    LastPtsMs_        = 0;
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
    int64_t dts           = pkt.dts_ms;
    int64_t pts           = pkt.pts_ms;

    if (dts < 0 && pts >= 0)
    {
        dts           = pts;
        used_fallback = true;
    }
    if (pts < 0 && dts >= 0)
    {
        pts           = dts;
        used_fallback = true;
    }

    if (dts < 0 || pts < 0)
    {
        const uint64_t step_ms = (FrameIntervalMs_ > 0 ? FrameIntervalMs_ : 1);
        if (!HasLastTimestamp_)
        {
            dts = 0;
            pts = 0;
        }
        else
        {
            dts = static_cast<int64_t>(LastDtsMs_ + step_ms);
            pts = dts;
        }
        used_fallback = true;
    }

    if (HasLastTimestamp_ && static_cast<uint64_t>(dts) <= LastDtsMs_)
    {
        dts           = static_cast<int64_t>(LastDtsMs_ + 1);
        used_fallback = true;
    }
    if (pts < dts)
    {
        pts           = dts;
        used_fallback = true;
    }

    LastDtsMs_        = static_cast<uint64_t>(dts);
    LastPtsMs_        = static_cast<uint64_t>(pts);
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
