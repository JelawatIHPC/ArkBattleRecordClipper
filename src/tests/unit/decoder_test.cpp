#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "decoder.h"

namespace {

constexpr int kWidth = 320;
constexpr int kHeight = 240;
constexpr int kFps = 30;
constexpr int kNumFrames = 5;

/* 创建测试视频文件用于解码
 *
 * 尝试多个编码器, 取首个可用的
 *
 * @param filename 输出视频文件路径
 * @return true 成功, false 无可用编码器
 */
bool CreateTestVideo(const std::string& filename) {
    const std::vector<std::string> encoder_names = {
        "libopenh264", "mpeg4"
    };

    const AVCodec* enc = nullptr;
    for (const auto& name : encoder_names) {
        enc = avcodec_find_encoder_by_name(name.c_str());
        if (enc) break;
    }
    if (!enc) return false;

    AVFormatContext* fmt_ctx = nullptr;
    avformat_alloc_output_context2(&fmt_ctx, nullptr, nullptr, filename.c_str());
    if (!fmt_ctx) return false;

    AVStream* stream = avformat_new_stream(fmt_ctx, nullptr);
    if (!stream) {
        avformat_free_context(fmt_ctx);
        return false;
    }
    stream->time_base = {1, kFps};

    AVCodecContext* enc_ctx = avcodec_alloc_context3(enc);
    if (!enc_ctx) {
        avformat_free_context(fmt_ctx);
        return false;
    }
    enc_ctx->width = kWidth;
    enc_ctx->height = kHeight;
    enc_ctx->time_base = {1, kFps};
    enc_ctx->framerate = {kFps, 1};
    enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    enc_ctx->gop_size = kNumFrames;

    if (avcodec_open2(enc_ctx, enc, nullptr) < 0) {
        avcodec_free_context(&enc_ctx);
        avformat_free_context(fmt_ctx);
        return false;
    }

    avcodec_parameters_from_context(stream->codecpar, enc_ctx);

    if (avio_open(&fmt_ctx->pb, filename.c_str(), AVIO_FLAG_WRITE) < 0) {
        avcodec_free_context(&enc_ctx);
        avformat_free_context(fmt_ctx);
        return false;
    }

    if (avformat_write_header(fmt_ctx, nullptr) < 0) {
        avio_closep(&fmt_ctx->pb);
        avcodec_free_context(&enc_ctx);
        avformat_free_context(fmt_ctx);
        return false;
    }

    AVFrame* frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = kWidth;
    frame->height = kHeight;
    av_frame_get_buffer(frame, 0);

    AVPacket* pkt = av_packet_alloc();

    for (int i = 0; i < kNumFrames; i++) {
        std::memset(frame->data[0], 128 + i * 10,
                    static_cast<size_t>(frame->linesize[0]) * kHeight);
        std::memset(frame->data[1], 128,
                    static_cast<size_t>(frame->linesize[1]) * kHeight / 2);
        std::memset(frame->data[2], 128,
                    static_cast<size_t>(frame->linesize[2]) * kHeight / 2);
        frame->pts = i;

        avcodec_send_frame(enc_ctx, frame);
        while (avcodec_receive_packet(enc_ctx, pkt) == 0) {
            pkt->stream_index = stream->index;
            av_packet_rescale_ts(pkt, enc_ctx->time_base, stream->time_base);
            av_interleaved_write_frame(fmt_ctx, pkt);
            av_packet_unref(pkt);
        }
    }

    avcodec_send_frame(enc_ctx, nullptr);
    while (avcodec_receive_packet(enc_ctx, pkt) == 0) {
        pkt->stream_index = stream->index;
        av_interleaved_write_frame(fmt_ctx, pkt);
        av_packet_unref(pkt);
    }

    av_write_trailer(fmt_ctx);

    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&enc_ctx);
    avio_closep(&fmt_ctx->pb);
    avformat_free_context(fmt_ctx);

    return true;
}

}  // namespace

/* ACDecoder 自动模式: 创建测试视频并解码一帧, 不出错即为成功
 */
TEST(ACDecoder, AutoCodecDecodesOneFrame) {
    const std::string filename = "test_decoder_input.mp4";

    ASSERT_TRUE(CreateTestVideo(filename)) << "无法创建测试视频文件";

    ACDecoder decoder(filename, ACDecoder::Codec::AUTO, ACDecoder::Format::NV12);
    AVFrame* frame = decoder.Decode();

    EXPECT_NE(nullptr, frame);
    // GetFormat 必须与实际解码帧格式一致 (构造期试解码保证回退后格式正确)
    EXPECT_EQ(decoder.GetFormat(), frame->format);
    EXPECT_EQ(kWidth, decoder.GetWidth());
    EXPECT_EQ(kHeight, decoder.GetHeight());
    EXPECT_FALSE(decoder.GetCodecName().empty());

    std::remove(filename.c_str());
}
