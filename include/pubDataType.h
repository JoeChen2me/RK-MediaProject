#ifndef PUBDATATYPE_H
#define PUBDATATYPE_H

#include <cstddef>

struct MappedBuffer
{
    void*  base   = nullptr;
    size_t length = 0;
};

struct v4l2FD_Info
{
    int    fd         = -1;
    size_t bufferSize = 0;
};

#endif  // PUBDATATYPE_H