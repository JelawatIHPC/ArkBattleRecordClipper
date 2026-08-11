extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include "decoder.h"

/* FFmpeg 编码器封装, 根据输入视频的参数来初始化, 接收 AVFrame 并完成编码和写入文件工作。
*/
class ACEncoder {
public:
    enum class Codec {
        AUTO     = 0,
        OPENH264 = 1,
        QSV      = 2,
        NVENC    = 3,
        AMF      = 4,
    };

    enum class Format {
        YUV420P = AV_PIX_FMT_YUV420P,
        NV12    = AV_PIX_FMT_NV12,
    };

    ACEncoder() = delete;
    ACEncoder(const ACEncoder&& other) = delete;

    ACEncoder(const std::string& output_filename, const ACDecoder* input_decoder, Codec codec, Format format, int64_t bit_rate);

    /* 编码并写入一帧, 支持加速 (减速无效) */
    bool Encode(AVFrame* frame, float speed = 1.0f);

    /* 写入文件尾 */
    void Close();

    /* 编码器需求的像素格式 */
    AVPixelFormat GetFormat() { return output_codec_ctx->pix_fmt; }

    /* 使用的编码器名 */
    const std::string GetCodecName() { return codec_name; }

    /* 打印 Encoder 信息 */
    friend std::ostream& operator<<(std::ostream& output, const ACEncoder& E) {
        return output;
    }

    /* 释放资源 */
    ~ACEncoder();

private:
    AVFormatContext* output_format_ctx{ nullptr };
    AVCodecContext*  output_codec_ctx{ nullptr };
    AVStream*        output_stream{ nullptr };
    AVPacket*        pkt{ nullptr };

    int64_t          pts{ 0 };
    std::string      codec_name;
};
