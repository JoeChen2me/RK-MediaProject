#ifndef ZLM_PUBLISHER_H
#define ZLM_PUBLISHER_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "rkmppenc.h"
#include "mk_mediakit.h"

class ZlmPublisher
{
   public:
    ZlmPublisher();
    ~ZlmPublisher();

    int      Init();
    int      InputPacketChunk(const EncPacketView& pkt);
    void     Close();
    uint64_t GetOutputFrameCount() const { return OutputFrameCount_; }
    uint64_t GetTimestampFallbackCount() const { return TimestampFallbackCount_; }

   private:
    struct TimestampNode
    {
        uint64_t dts_ms = 0;
        uint64_t pts_ms = 0;
    };

    static void OnH264Frame(void* user_data, mk_h264_splitter splitter, const char* data, int size);
    void        OnH264FrameInternal(const char* data, int size);

    bool NormalizeTimestamp(const EncPacketView& pkt, uint64_t* out_dts_ms, uint64_t* out_pts_ms);
    bool IsAnnexBStartCode(const uint8_t* data, size_t len) const;
    bool IsConfigFrame(const uint8_t* data, size_t len) const;

    mk_media         Media_    = nullptr;
    mk_h264_splitter Splitter_ = nullptr;

    std::deque<TimestampNode> TimestampQueue_;
    std::vector<char>         Scratch_;

    uint64_t LastDtsMs_              = 0;
    uint64_t LastPtsMs_              = 0;
    uint64_t OutputFrameCount_       = 0;
    uint64_t TimestampFallbackCount_ = 0;
    uint64_t FrameIntervalMs_        = 1000 / 30;
    bool     HasFedAnyChunk_         = false;
    bool     Initialized_            = false;
    bool     HasLastTimestamp_       = false;
};

#endif  // ZLM_PUBLISHER_H
