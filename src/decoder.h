#pragma once

#include <iostream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libavutil/opt.h>
}


class ACDecoder {
    /* FFmpeg 解码器封装, 读取视频, 解码出 AVFrame。
    */
public:

    enum class Codec {
        DXVA2 = 1,
        CPU   = 2,
    };

    ACDecoder() = delete;
    ACDecoder(const ACDecoder&& other) = delete;

    ACDecoder(const std::string& filename, Codec codec);

    /* 按视频播放顺序解码下一个视频帧, 存储到内部分配的 AVFrame 中。
     * 解码新的视频帧时, 上一帧立即释放。
     * 返回解码视频帧的 AVFrame 指针, 若视频结束则返回 nullptr.
    */
    AVFrame* Decode();

    /* 根据时间戳 (第几秒), 快速跳转到该时间戳之前的最后一个关键帧
    */
    bool Seek(int second);

    /* 设置是否只输出关键帧 (仅影响后续 Decode 调用, 无需重新初始化解码器)
     *
     * 通过 skip_frame = AVDISCARD_NONKEY 实现, 非关键帧在解码器内被直接丢弃。
     *
     * @param enable true 时只输出关键帧, false 时恢复输出所有帧
     */
    void SetKeyframeOnly(bool enable);

    /**
    * 按视频播放顺序解码下一个视频帧, 存储到指定的 AVFrame 中。
    *
    * @param frame AVFrame pointer
    *
    * @retval  0 读取成功
    * @retval -1 视频结束
    */
    int Decode(AVFrame* frame);

    /* 视频帧宽度 */
    int GetWidth() const;

    /* 视频帧高度 */
    int GetHeight() const;

    /* 视频时间基 */
    AVRational GetTimebase() const;

    /* 视频平均帧率 */
    AVRational GetAvgFramerate() const;

    /* 视频平均码率 */
    int64_t GetAvgBitrate() const;

    /* 视频总帧数 */
    int64_t GetFrameCount() const;

    /* 解码器输出格式 */
    AVPixelFormat GetFormat() const;

    /* 打印 Decoder 信息 */
    friend std::ostream& operator<<(std::ostream& output, const ACDecoder& E);

    ~ACDecoder();

private:
    AVFormatContext* input_format_ctx{ nullptr };
    AVCodecContext*  input_codec_ctx{ nullptr };
    AVStream*        input_stream{ nullptr };
    AVFrame*         hardware_frame{ nullptr };
    AVFrame*         ret_frame{ nullptr };
    AVPacket*        pkt{ nullptr };

    AVPixelFormat    pix_fmt{ AV_PIX_FMT_NONE };

    /* 向解码器发送一个新的数据包
     * Return 0 if OK, < 0 on end of file.
    */
    int SendPacket();

}; // class ACDecoder
