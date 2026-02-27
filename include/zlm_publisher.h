#ifndef ZLM_PUBLISHER_H
#define ZLM_PUBLISHER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "rkmppenc.h"
#include "mk_mediakit.h"

struct ZlmPublishConfig
{
    uint16_t    rtsp_port = 8554;
    uint16_t    http_port = 8000;
    uint16_t    rtc_port  = 8001;
    std::string vhost     = "__defaultVhost__";
    std::string app       = "live";
    std::string stream    = "camera";
};

class ZlmPublisher
{
   public:
    ZlmPublisher();
    ~ZlmPublisher();

    int         Init(const ZlmPublishConfig& cfg = {});
    void        SetExpectedFps(uint32_t fps);
    int         InputPacketChunk(const EncPacketView& pkt);
    void        Close();
    std::string GetRtspUrl() const;
    std::string GetWebRtcApiUrl() const;
    uint64_t    GetOutputFrameCount() const { return OutputFrameCount_; }
    uint64_t    GetTimestampFallbackCount() const { return TimestampFallbackCount_; }

   private:
    struct NaluRange
    {
        const uint8_t* data = nullptr;
        size_t         len  = 0;
    };

    bool NormalizeTimestamp(const EncPacketView& pkt, uint64_t* out_dts_ms, uint64_t* out_pts_ms);
    bool IsAnnexBStartCode(const uint8_t* data, size_t len) const;
    bool FindAnnexBPrefix(const uint8_t* data, size_t len, size_t pos, size_t* prefix_len) const;
    bool SplitAnnexBNalus(const uint8_t* data, size_t len, std::vector<NaluRange>* out_nalus) const;

    mk_media         Media_         = nullptr;
    ZlmPublishConfig PublishConfig_ = {};
    uint16_t         RtspPort_      = 0;
    uint16_t         HttpPort_      = 0;
    uint16_t         RtcPort_       = 0;

    uint64_t LastDtsUs_              = 0;
    uint64_t LastPtsUs_              = 0;
    uint64_t LastDtsMs_              = 0;
    uint64_t LastPtsMs_              = 0;
    uint64_t OutputFrameCount_       = 0;
    uint64_t TimestampFallbackCount_ = 0;
    uint64_t FrameIntervalUs_        = 1000000 / 30;
    bool     Initialized_            = false;
    bool     HasLastTimestamp_       = false;
};

#endif  // ZLM_PUBLISHER_H
