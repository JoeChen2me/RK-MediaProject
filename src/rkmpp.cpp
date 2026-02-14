
#include "rkmpp.h"

MppInstance::MppInstance()
{
    mpp_api = nullptr;
    mpp_ctx = nullptr;
}

int MppInstance::MppInit()
{
    // 创建 MPP 上下文和 API
    if (mpp_create(&mpp_ctx, &mpp_api) != 0)
    {
        mpp_ctx = nullptr;
        mpp_api = nullptr;
        return -1;  // 创建失败
    }
    // 初始化 MPP 上下文，设置为解码器并指定编码格式为 MJPEG
    if (mpp_init(mpp_ctx, MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG) != 0)
    {
        mpp_destroy(mpp_ctx);
        mpp_ctx = nullptr;
        mpp_api = nullptr;
        return -1;  // 初始化失败
    }
    return 0;
}

MppInstance::~MppInstance()
{
    // 销毁 MPP 上下文
    if (mpp_ctx)
    {
        mpp_destroy(mpp_ctx);
        mpp_ctx = nullptr;
        mpp_api = nullptr;
    }
}
