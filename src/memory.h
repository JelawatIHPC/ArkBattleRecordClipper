#pragma once

#include <vector>
#include <unordered_set>

extern "C" {
#include <libavutil/frame.h>
#include <libavcodec/packet.h>
}

class ACPacketPool {
public:
    ACPacketPool();

    ACPacketPool(const ACPacketPool& other) = delete;

    /* 分配 AVPacket */
    AVPacket* Allocate();

    /* 释放 AVPacket */
    void Free(AVPacket* p);

    /* 获取默认内存池 */
    static ACPacketPool& DefaultPool();

    ~ACPacketPool();
private:
    std::vector<AVPacket*> free_list;
    std::unordered_set<AVPacket*> allocated_list;
}; // ACPacketPool


class ACFramePool {
    /* AVFrame 分配池
    */
public:
    ACFramePool();

    /* 分配单个 AVFrame, 失败则返回 nullptr */
    AVFrame* Allocate();

    /* 释放 AVFrame */
    void Free(AVFrame* frame);

    /* 获取默认内存池 */
    static ACFramePool& DefaultPool();

    ~ACFramePool();

private:
    std::vector<AVFrame*> free_list;
    std::unordered_set<AVFrame*> allocated_list;
};