#include "zlm_publisher.h"

#include <cstring>
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

    Splitter_ = mk_h264_splitter_create(OnH264Frame, this, 0);
    if (!Splitter_)
    {
        Close();
        return -1;
    }

    Initialized_ = true;
    return 0;
}

int ZlmPublisher::InputPacketChunk(const EncPacketView& pkt)
{
    try
    {
        if (!Initialized_ || !Splitter_)
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

        const uint8_t* bytes          = static_cast<const uint8_t*>(pkt.data);
        const bool     has_start_code = IsAnnexBStartCode(bytes, pkt.len);
        if (!has_start_code && (pkt.is_extra || !HasFedAnyChunk_))
        {
            return -1;
        }

        if (!pkt.is_extra)
        {
            uint64_t dts_ms = 0;
            uint64_t pts_ms = 0;
            if (!NormalizeTimestamp(pkt, &dts_ms, &pts_ms))
            {
                return -1;
            }
            if (TimestampQueue_.empty() || TimestampQueue_.back().dts_ms != dts_ms ||
                TimestampQueue_.back().pts_ms != pts_ms)
            {
                TimestampQueue_.push_back(TimestampNode{dts_ms, pts_ms});
            }
        }

        Scratch_.resize(pkt.len + 1);
        std::memcpy(Scratch_.data(), pkt.data, pkt.len);
        Scratch_[pkt.len] = '\0';
        mk_h264_splitter_input_data(Splitter_, Scratch_.data(), static_cast<int>(pkt.len));
        HasFedAnyChunk_ = true;
        return 0;
    }
    catch (...)
    {
        return -1;
    }
}

void ZlmPublisher::Close()
{
    if (Splitter_)
    {
        mk_h264_splitter_release(Splitter_);
        Splitter_ = nullptr;
    }
    if (Media_)
    {
        mk_media_release(Media_);
        Media_ = nullptr;
    }

    mk_stop_all_server();

    TimestampQueue_.clear();
    Scratch_.clear();
    HasFedAnyChunk_ = false;
    Initialized_    = false;
}

void ZlmPublisher::OnH264Frame(void* user_data, mk_h264_splitter splitter, const char* data,
                               int size)
{
    (void)splitter;
    if (!user_data)
    {
        return;
    }
    static_cast<ZlmPublisher*>(user_data)->OnH264FrameInternal(data, size);
}

void ZlmPublisher::OnH264FrameInternal(const char* data, int size)
{
    if (!Media_ || !data || size <= 0)
    {
        return;
    }

    const uint8_t* bytes     = reinterpret_cast<const uint8_t*>(data);
    const bool     is_config = IsConfigFrame(bytes, static_cast<size_t>(size));
    uint64_t       dts_ms    = 0;
    uint64_t       pts_ms    = 0;

    if (!is_config && !TimestampQueue_.empty())
    {
        const TimestampNode node = TimestampQueue_.front();
        TimestampQueue_.pop_front();
        dts_ms = node.dts_ms;
        pts_ms = node.pts_ms;
    }
    else if (!is_config)
    {
        if (HasLastTimestamp_)
        {
            dts_ms = LastDtsMs_ + FrameIntervalMs_;
            pts_ms = dts_ms;
        }
        TimestampFallbackCount_++;
    }
    else
    {
        if (HasLastTimestamp_)
        {
            dts_ms = LastDtsMs_;
            pts_ms = LastPtsMs_;
        }
    }

    if (!is_config)
    {
        LastDtsMs_        = dts_ms;
        LastPtsMs_        = pts_ms;
        HasLastTimestamp_ = true;
    }

    mk_frame frame = mk_frame_create(MKCodecH264, dts_ms, pts_ms, data, static_cast<size_t>(size),
                                     nullptr, nullptr);
    if (!frame)
    {
        return;
    }

    const int ret = mk_media_input_frame(Media_, frame);
    mk_frame_unref(frame);
    if (ret)
    {
        OutputFrameCount_++;
    }
}

bool ZlmPublisher::NormalizeTimestamp(const EncPacketView& pkt, uint64_t* out_dts_ms,
                                      uint64_t* out_pts_ms)
{
    if (!out_dts_ms || !out_pts_ms)
    {
        return false;
    }

    bool    fallback = false;
    int64_t dts      = pkt.dts_ms;
    int64_t pts      = pkt.pts_ms;

    if (dts < 0 && pts >= 0)
    {
        dts      = pts;
        fallback = true;
    }
    if (pts < 0 && dts >= 0)
    {
        pts      = dts;
        fallback = true;
    }
    if (dts < 0 || pts < 0)
    {
        if (HasLastTimestamp_)
        {
            dts = static_cast<int64_t>(LastDtsMs_ + FrameIntervalMs_);
            pts = dts;
        }
        else
        {
            dts = 0;
            pts = 0;
        }
        fallback = true;
    }

    if (HasLastTimestamp_ && static_cast<uint64_t>(dts) <= LastDtsMs_)
    {
        dts      = static_cast<int64_t>(LastDtsMs_ + 1);
        fallback = true;
    }
    if (pts < dts)
    {
        pts      = dts;
        fallback = true;
    }

    if (fallback)
    {
        TimestampFallbackCount_++;
    }

    *out_dts_ms = static_cast<uint64_t>(dts);
    *out_pts_ms = static_cast<uint64_t>(pts);
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

bool ZlmPublisher::IsConfigFrame(const uint8_t* data, size_t len) const
{
    if (!IsAnnexBStartCode(data, len))
    {
        return false;
    }

    size_t prefix = 0;
    if (len >= 4 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x01)
    {
        prefix = 4;
    }
    else
    {
        prefix = 3;
    }
    if (len <= prefix)
    {
        return false;
    }

    const uint8_t nal_type = data[prefix] & 0x1Fu;
    return nal_type == 7u || nal_type == 8u;
}
