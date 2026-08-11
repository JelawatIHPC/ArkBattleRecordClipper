#include "memory.h"

ACPacketPool __ac_default_packet_pool;
ACFramePool  __ac_default_frame_pool;

ACPacketPool::ACPacketPool() {
}

AVPacket* ACPacketPool::Allocate() {
    if (!free_list.empty()) {
        AVPacket* ret = free_list.back();
        free_list.pop_back();
        allocated_list.insert(ret);
        return ret;
    }
    else {
        AVPacket* ret = av_packet_alloc();
        allocated_list.insert(ret);
        return ret;
    }
}

void ACPacketPool::Free(AVPacket* p) {
    if (allocated_list.contains(p)) {
        av_packet_unref(p);
        free_list.push_back(p);
        allocated_list.erase(p);
    }
}

ACPacketPool::~ACPacketPool() {
    for (auto p : free_list) {
        av_packet_free(&p);
    }
    for (auto p : allocated_list) {
        av_packet_free(&p);
    }
}

ACPacketPool& ACPacketPool::DefaultPool() {
    return __ac_default_packet_pool;
}

ACFramePool::ACFramePool() {
}

AVFrame* ACFramePool::Allocate() {
    if (!free_list.empty()) {
        AVFrame* ret = free_list.back();
        free_list.pop_back();
        allocated_list.insert(ret);
        return ret;
    }
    else {
        AVFrame* ret = av_frame_alloc();
        if (ret) allocated_list.insert(ret);
        return ret;
    }
}

void ACFramePool::Free(AVFrame* frame) {
    if (allocated_list.contains(frame)) {
        av_frame_unref(frame);
        free_list.push_back(frame);
        allocated_list.erase(frame);
    }
}

ACFramePool::~ACFramePool() {
    for (auto p : free_list) {
        av_frame_free(&p);
    }
    for (auto p : allocated_list) {
        av_frame_free(&p);
    }
}

ACFramePool& ACFramePool::DefaultPool() {
    return __ac_default_frame_pool;
}