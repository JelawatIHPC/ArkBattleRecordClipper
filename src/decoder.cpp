#include "decoder.h"

#include <format>
#include <stdexcept>
#include <vector>


void __ACDecoder_Debug_PutHwFrame(AVFrame* hardware_frame) {
    AVPixelFormat* formats;
    std::string error_msg;
    if (av_hwframe_transfer_get_formats(hardware_frame->hw_frames_ctx, AV_HWFRAME_TRANSFER_DIRECTION_FROM, &formats, 0) == 0) {
        error_msg += "支持的像素格式列表为\n";
        for (auto i = 0; formats[i] != AV_PIX_FMT_NONE; i++)
            error_msg += std::format("  {}\n", int(formats[i]));
        av_free(formats);
    }
    else {
        error_msg += "像素格式支持列表获取失败。\n";
    }
    error_msg += "硬件帧信息\n";
    error_msg += std::format("  width: {}\n  height: {}\n  pts: {}\n  format:{}\n",
        hardware_frame->width, hardware_frame->height,
        hardware_frame->pts, hardware_frame->format
    );
    throw std::runtime_error(error_msg);
}


int ACDecoder::GetWidth() const {
    return input_codec_ctx->width;
}

int ACDecoder::GetHeight() const {
    return input_codec_ctx->height;
}

AVRational ACDecoder::GetTimebase() const {
    return input_stream->time_base;
}

AVRational ACDecoder::GetAvgFramerate() const {
    return input_stream->avg_frame_rate;
}

int64_t ACDecoder::GetAvgBitrate() const {
    return input_codec_ctx->bit_rate;
}

int64_t ACDecoder::GetFrameCount() const {
    if (input_stream->nb_frames <= 0) return 1;
    return input_stream->nb_frames;
}

AVPixelFormat ACDecoder::GetFormat() const {
    return pix_fmt;
}

ACDecoder::ACDecoder(const std::string& filename, Codec codec, Format format) {
    auto_codec = codec == Codec::AUTO;
    int ret;
    // 打开输入文件
    if ((ret = avformat_open_input(&input_format_ctx, filename.c_str(), nullptr, nullptr)) != 0) {
        char buf[1024] = "\0";
        av_strerror(ret, buf, 1024);
        throw std::runtime_error(std::format("无法打开输入文件: {} [FFmpeg: {}] {}", filename, ret, buf));
    }

    // 查找流信息
    if (avformat_find_stream_info(input_format_ctx, nullptr) < 0) {
        throw std::runtime_error("无法获取输入文件的流信息");
    }

    // 查找视频流
    for (unsigned int i = 0; i < input_format_ctx->nb_streams; i++) {
        if (input_format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            input_stream = input_format_ctx->streams[i];
            break;
        }
    }
    if (!input_stream) {
        throw std::runtime_error("未找到视频流");
    }

    // 获取视频编解码器
    const AVCodec* av_codec = avcodec_find_decoder(input_stream->codecpar->codec_id);
    if (!av_codec) {
        throw std::runtime_error("无法找到解码器");
    }

    // ACDecoder::Codec -> 名称
    const std::vector<std::string> CODEC_NAME = {
        "auto",     // AUTO, should not be passed to FFmpeg
        "dxva2",    // DXVA2
        "cpu",      // CPU
    };

    std::vector<Codec> codec_order = {
        Codec::DXVA2, Codec::CPU
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

    // 按列表顺序尝试打开解码器
    bool succeeded = false;
    for (auto codec_type : codec_order) {
        for (auto pix_fmt_try : format_order) {

            // 创建解码器上下文
            input_codec_ctx = avcodec_alloc_context3(av_codec);
            if (!input_codec_ctx) continue;

            // 复制编解码器参数
            if (avcodec_parameters_to_context(input_codec_ctx, input_stream->codecpar) < 0) {
                avcodec_free_context(&input_codec_ctx);
                continue;
            }

            if (codec_type == Codec::DXVA2) {
                input_codec_ctx->extra_hw_frames = (int)ceil(av_q2d(input_stream->avg_frame_rate)) - 17;
                if (input_codec_ctx->extra_hw_frames < 0)
                    input_codec_ctx->extra_hw_frames = 103;

                AVBufferRef* hw_dev = nullptr;
                if (av_hwdevice_ctx_create(&hw_dev, AV_HWDEVICE_TYPE_DXVA2, NULL, NULL, NULL) < 0) {
                    avcodec_free_context(&input_codec_ctx);
                    continue;
                }

                // 预先创建硬件帧池。某些无可用 DXVA2 表面的环境会在
                // avcodec_receive_frame() 内部才失败甚至触发 SEH,
                // 这里提前把该硬件候选判定为不可用。
                AVBufferRef* hw_frames = av_hwframe_ctx_alloc(hw_dev);
                if (!hw_frames) {
                    av_buffer_unref(&hw_dev);
                    avcodec_free_context(&input_codec_ctx);
                    continue;
                }
                AVHWFramesContext* hw_frames_ctx =
                    reinterpret_cast<AVHWFramesContext*>(hw_frames->data);
                hw_frames_ctx->format = AV_PIX_FMT_DXVA2_VLD;
                hw_frames_ctx->sw_format = AV_PIX_FMT_NV12;
                hw_frames_ctx->width = input_stream->codecpar->width;
                hw_frames_ctx->height = input_stream->codecpar->height;
                hw_frames_ctx->initial_pool_size = 1;
                if (av_hwframe_ctx_init(hw_frames) < 0) {
                    av_buffer_unref(&hw_frames);
                    av_buffer_unref(&hw_dev);
                    avcodec_free_context(&input_codec_ctx);
                    continue;
                }
                av_buffer_unref(&hw_frames);
                input_codec_ctx->hw_device_ctx = hw_dev;
            }

            pix_fmt = static_cast<AVPixelFormat>(pix_fmt_try);

            if (avcodec_open2(input_codec_ctx, av_codec, nullptr) == 0) {
                succeeded = true;
                codec_name = CODEC_NAME[(size_t)codec_type];
                // CPU 解码器输出格式由解码器决定, 读取实际值
                if (codec_type == Codec::CPU) {
                    pix_fmt = input_codec_ctx->pix_fmt;
                }
                break;
            }

            avcodec_free_context(&input_codec_ctx);
        }
        if (succeeded) break;
    }
    if (!succeeded) {
        throw std::runtime_error("找不到可用的视频解码器");
    }

    // 创建数据包对象
    pkt = av_packet_alloc();
    if (!pkt) {
        throw std::runtime_error("无法为数据包对象分配内存");
    }

    // 创建帧对象
    ret_frame = av_frame_alloc();
    hardware_frame = av_frame_alloc();
    if (!ret_frame || !hardware_frame) {
        throw std::runtime_error("分配帧内存失败");
    }
}


AVFrame* ACDecoder::Decode() {
    int ret;
    do {
        ret = avcodec_receive_frame(input_codec_ctx, hardware_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            try {
                if (SendPacket() < 0) return nullptr;   // 视频结束
            }
            catch (const std::runtime_error&) {
                if (input_codec_ctx->hw_device_ctx && auto_codec && !returned_frame) {
                    FallbackToCpu();
                    return Decode();
                }
                throw;
            }
            continue;
        }
        else if (ret < 0) {
            if (input_codec_ctx->hw_device_ctx && auto_codec && !returned_frame) {
                FallbackToCpu();
                return Decode();
            }
            throw std::runtime_error("从解码器接收帧时出现错误");
        }
    } while (ret < 0);

    if (input_codec_ctx->hw_device_ctx == nullptr) {
        // 非硬件解码器，直接返回解码帧
        returned_frame = true;
        return hardware_frame;
    }

    av_frame_unref(ret_frame);
    ret_frame->format = pix_fmt;
    ret_frame->width = hardware_frame->width;
    ret_frame->height = hardware_frame->height;
    if (av_hwframe_map(ret_frame, hardware_frame, 0) < 0) {
        if (auto_codec && !returned_frame) {
            FallbackToCpu();
            return Decode();
        }
        std::cerr << "解码器无法将硬件帧映射到内存：";
        __ACDecoder_Debug_PutHwFrame(hardware_frame);
        throw std::exception();
    }
    av_frame_unref(hardware_frame);

    returned_frame = true;
    return ret_frame;
}


bool ACDecoder::Seek(int second) {
    int ret = av_seek_frame(
        input_format_ctx, input_stream->index,
        av_rescale_q(second, AVRational{ 1,1 }, GetTimebase()),
        AVSEEK_FLAG_BACKWARD
    );
    avcodec_flush_buffers(input_codec_ctx);
    return (ret >= 0);
}


void ACDecoder::SetKeyframeOnly(bool enable) {
    input_codec_ctx->skip_frame = enable ? AVDISCARD_NONKEY : AVDISCARD_DEFAULT;
}


int ACDecoder::Decode(AVFrame* frame) {

    if (!frame) {
        throw std::runtime_error("解码器接收帧内存未分配");
    }

    int ret;
    while ((ret = avcodec_receive_frame(input_codec_ctx, hardware_frame)) == AVERROR(EAGAIN)) {
        try {
            if (SendPacket() < 0) return -1;
        }
        catch (const std::runtime_error&) {
            if (input_codec_ctx->hw_device_ctx && auto_codec && !returned_frame) {
                FallbackToCpu();
                return Decode(frame);
            }
            throw;
        }
    }
    if (ret < 0) {
        if (ret == AVERROR_EOF) return -1;
        if (input_codec_ctx->hw_device_ctx && auto_codec && !returned_frame) {
            FallbackToCpu();
            return Decode(frame);
        }
        throw std::runtime_error("从解码器接收帧时出现错误");
    }

    if (input_codec_ctx->hw_device_ctx == nullptr) {
        // 非硬件解码器，直接返回解码帧
        av_frame_move_ref(frame, hardware_frame);
        returned_frame = true;
        return 0;
    }

    // 从硬件解码器读取帧
    frame->format = pix_fmt;
    frame->width = hardware_frame->width;
    frame->height = hardware_frame->height;
    if (av_hwframe_map(frame, hardware_frame, 0) < 0) {
        if (auto_codec && !returned_frame) {
            FallbackToCpu();
            return Decode(frame);
        }
        throw std::runtime_error("解码器无法将硬件帧映射到内存");
    }
    av_frame_unref(hardware_frame);

    returned_frame = true;
    return 0;
}


void ACDecoder::FallbackToCpu() {
    if (av_seek_frame(input_format_ctx, input_stream->index, 0, AVSEEK_FLAG_BACKWARD) < 0) {
        throw std::runtime_error("硬件解码失败且无法回退到 CPU 解码器");
    }
    avformat_flush(input_format_ctx);
    avcodec_free_context(&input_codec_ctx);

    const AVCodec* av_codec = avcodec_find_decoder(input_stream->codecpar->codec_id);
    if (!av_codec) {
        throw std::runtime_error("无法找到 CPU 解码器");
    }

    input_codec_ctx = avcodec_alloc_context3(av_codec);
    if (!input_codec_ctx ||
        avcodec_parameters_to_context(input_codec_ctx, input_stream->codecpar) < 0 ||
        avcodec_open2(input_codec_ctx, av_codec, nullptr) < 0) {
        avcodec_free_context(&input_codec_ctx);
        throw std::runtime_error("无法打开 CPU 解码器");
    }

    pix_fmt = input_codec_ctx->pix_fmt;
    codec_name = "cpu";
    av_packet_unref(pkt);
    av_frame_unref(hardware_frame);
    av_frame_unref(ret_frame);
}


std::ostream& operator<<(std::ostream& output, const ACDecoder& E) { return output; }


ACDecoder::~ACDecoder() {
    // 释放帧
    av_frame_free(&ret_frame);
    av_frame_free(&hardware_frame);
    av_packet_free(&pkt);

    // 释放解码器上下文
    if (input_codec_ctx) {
        avcodec_free_context(&input_codec_ctx);
    }

    // 释放格式上下文
    if (input_format_ctx) {
        avformat_close_input(&input_format_ctx);
    }
}


int ACDecoder::SendPacket() {
    int ret;
    av_packet_unref(pkt);
    while ((ret = av_read_frame(input_format_ctx, pkt)) >= 0) {
        // 找到视频流的数据包
        if (pkt->stream_index == input_stream->index) {
            // 发送数据包到解码器
            if (avcodec_send_packet(input_codec_ctx, pkt) < 0) {
                av_packet_unref(pkt);
                throw std::runtime_error("无法发送数据包到解码器");
            }
            return 0;
        }
        av_packet_unref(pkt);
    }
    return ret;
}
