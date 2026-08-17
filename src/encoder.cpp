#include "encoder.h"

#include <vector>
#include <format>
#include <thread>
#include <stdexcept>

ACEncoder::ACEncoder(const std::string& output_filename, const ACDecoder* input_decoder, 
                     Codec codec, Format format, int64_t bit_rate)
{
    // ACEncoder::Codec -> FFmpeg codec name
    const std::vector<std::string> CODEC_NAME = {
        "auto",         // AUTO, should not be passed to FFmpeg
        "libopenh264",  // OPENH264
        "h264_qsv",     // QSV
        "h264_nvenc",   // NVENC
        "h264_amf"      // AMF
    };

    std::vector<Codec> codec_order = {
        Codec::QSV, Codec::NVENC, Codec::AMF, Codec::OPENH264
    };
    std::vector<Format> format_order = {
        Format::NV12, Format::YUV420P
    };
    // 根据传入的 codec 和 format 调整初始化顺序列表
    if (codec != Codec::AUTO) {
        for (size_t i = 0; i < codec_order.size(); i++)
            if (codec_order[i] == codec) {
                if (i != 0) std::swap(codec_order[i], codec_order[0]);
                break;
            }
    }
    for (size_t i = 0; i < format_order.size(); i++)
        if (format_order[i] == format) {
            if (i != 0) std::swap(format_order[i], format_order[0]);
            break;
        }

    // 创建输出格式上下文
    avformat_alloc_output_context2(&output_format_ctx, nullptr, nullptr, output_filename.c_str());
    if (!output_format_ctx) {
        throw std::runtime_error("无法创建输出格式上下文");
    }

    // 创建视频流
    output_stream = avformat_new_stream(output_format_ctx, nullptr);
    if (!output_stream) {
        throw std::runtime_error("无法创建视频流");
    }

    // 设置输出流的时间基准
    output_stream->time_base = input_decoder->GetTimebase();

    // 按列表顺序尝逝打开编码器
    bool succeeded = false;
    for (auto codec_type : codec_order) {

        std::string name = CODEC_NAME[(size_t)codec_type];

        for (auto pix_fmt : format_order) {

            AVPixelFormat format = static_cast<AVPixelFormat>(pix_fmt);

            // 获取输出编解码器
            const AVCodec* output_codec = avcodec_find_encoder_by_name(name.c_str());
            if (!output_codec) continue;

            // 创建编码器上下文
            output_codec_ctx = avcodec_alloc_context3(output_codec);
            if (!output_codec_ctx) continue;

            // 设置编码器参数
            output_codec_ctx->bit_rate = bit_rate + (bit_rate >> 4);    // *17/16
            output_codec_ctx->rc_max_rate = output_codec_ctx->bit_rate * 2;
            output_codec_ctx->width = input_decoder->GetWidth();
            output_codec_ctx->height = input_decoder->GetHeight();
            output_codec_ctx->time_base = input_decoder->GetTimebase();
            output_codec_ctx->framerate = input_decoder->GetAvgFramerate();
            output_codec_ctx->pix_fmt = format;
            output_codec_ctx->gop_size = (int)round(av_q2d(output_codec_ctx->framerate));
            output_codec_ctx->max_b_frames = 1;

            if (codec_type == Codec::OPENH264) {
                output_codec_ctx->thread_count = std::thread::hardware_concurrency();
            }

            if (avcodec_open2(output_codec_ctx, output_codec, 0) == 0) {
                succeeded = true;
                codec_name = name;
                break;
            }
        }
        if (succeeded) break;
    }
    if (!succeeded) {
        throw std::runtime_error("找不到可用的视频编码器");
    }
    
    // 复制编码器参数到流
    if (avcodec_parameters_from_context(output_stream->codecpar, output_codec_ctx) < 0) {
        throw std::runtime_error("无法复制编码器参数到流");
    }

    // 写入文件头
    if (avio_open(&output_format_ctx->pb, output_filename.c_str(), AVIO_FLAG_WRITE) < 0) {
        throw std::runtime_error(std::format("无法打开输出文件: {}", output_filename.c_str()));
    }

    if (avformat_write_header(output_format_ctx, nullptr) < 0) {
        throw std::runtime_error("无法写入文件头");
    }

    // 创建数据包对象
    pkt = av_packet_alloc();
    if (!pkt) {
        throw std::runtime_error("无法为数据包对象分配内存");
    }
}

bool ACEncoder::Encode(AVFrame* frame, float speed) {

    if (frame->format != output_codec_ctx->pix_fmt) {
        throw std::runtime_error("编码器接收到的视频帧格式与预设不符");
    }

    // 帧采样策略: 当传入视频帧要求加速时, 并不实际减少帧的持续时间, 而是把减少的时间存进 remainder
    // 当传入新的视频帧加速后的持续时间小于 remainder 时, 丢弃该帧, 并扣除相应 remainder
    if (speed > 1.0f) {
        double duration = frame->duration * av_q2d(output_codec_ctx->time_base) / speed;
        if (duration < remainder) {
            remainder -= duration;
            return true;
        }
        else {
            remainder += frame->duration * av_q2d(output_codec_ctx->time_base) - duration;
        }
    }

    frame->pts = pts;
    pts += frame->duration;
    
    int ret;
    while ((ret = avcodec_send_frame(output_codec_ctx, frame)))
    {
        if (ret == AVERROR(EAGAIN))
        {
            ret = avcodec_receive_packet(output_codec_ctx, pkt);
            if (ret == AVERROR(EAGAIN))
                continue;
            if (ret < 0) {
                throw std::runtime_error(std::format("从编码器读取数据包时发生错误: [FFmpeg: {}]", ret));
            }
            // 写入数据包
            pkt->stream_index = output_stream->index;
            av_packet_rescale_ts(pkt, output_codec_ctx->time_base, output_stream->time_base);
            av_interleaved_write_frame(output_format_ctx, pkt);
        }
        else {
            throw std::runtime_error(std::format("向编码器发送视频帧时发生错误: [FFmpeg: {}]", ret));
        }
    }

    return true;
}

void ACEncoder::Close() {
    // 发送一个空帧来刷新编码器
    avcodec_send_frame(output_codec_ctx, nullptr);

    while (avcodec_receive_packet(output_codec_ctx, pkt) == 0) {
        pkt->stream_index = output_stream->index;
        av_interleaved_write_frame(output_format_ctx, pkt);
        av_packet_unref(pkt);
    }

    // 写入文件尾
    av_write_trailer(output_format_ctx);
}

ACEncoder::~ACEncoder() {
    if (pkt) {
        av_packet_free(&pkt);
    }

    if (output_codec_ctx) {
        avcodec_free_context(&output_codec_ctx);
    }

    if (output_format_ctx) {
        if (output_format_ctx->pb) {
            avio_closep(&output_format_ctx->pb);
        }
        avformat_free_context(output_format_ctx);
    }
}