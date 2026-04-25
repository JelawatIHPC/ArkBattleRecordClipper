#include <string>
#include <memory>
#include <vector>
#include <chrono>
#include <thread>
#include <span>
#include <stdexcept>

#ifdef _DEBUG
#include <fstream>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include "ACEncoder.h"
#include "ACDecoder.h"
#include "ACDetector.h"
#include "ACMemory.h"
#include "ACLocator.h"
#include "core.h"

#define POW2(x) ((x)*(x))

// 向控制台打印一次进度的最短间隔 (ms)
constexpr int64_t DISPLAY_INTERVAL_MS = 333;

// 全局进度指标存储
static ProgressMetrics g_progress_metrics;

struct DetectedFrame {
    AVFrame* frame;
    int64_t pts;
    int64_t duration;
    FrameState state;
};


/* 毫秒计时器, 用于间隔执行操作。
*/
class ACMsTimer {
    using Clock = std::chrono::system_clock;
    using Ms = std::chrono::milliseconds;
public:
    ACMsTimer(uint32_t ms = 1000) noexcept : interval(ms) {
        Reset();
    }

    /* 距离上次调用返回 true 大于指定毫秒数时, 调用返回 true, 否则总是返回 false
    */
    bool Tick() noexcept {
        auto now = Clock::now();
        if (std::chrono::duration_cast<Ms>(now - last).count() >= interval) {
            last = now;
            return true;
        }
        return false;
    }

    /* 返回计时开始到现在过去的总毫秒数
    */
    int64_t Count() const noexcept {
        auto now = Clock::now();
        return std::chrono::duration_cast<Ms>(now - start).count();
    }

    /* 重置计时
    */
    void Reset() noexcept {
        start = last = Clock::now();
    }
private:
    Clock::time_point start;
    Clock::time_point last;
    uint32_t interval{ 1000 };
};


// CPU 像素格式转换器: 使用 libswscale 在 CPU 上把 AVFrame 从一种像素格式转换为另一种。
// 初始化时可以指定输入/输出的 AVPixelFormat 以及宽高。
class ACCPUTranscoder {
public:
    ACCPUTranscoder(AVPixelFormat in_fmt, AVPixelFormat out_fmt, int w, int h)
        : in_fmt(in_fmt), out_fmt(out_fmt), width(w), height(h)
    {
        Reinit(in_fmt, out_fmt, width, height);
    }

    // Transcode: 接收一个输入 AVFrame (只读), 返回一个新分配的 AVFrame*（调用者负责释放）
    // 如果转换失败返回 nullptr。
    AVFrame* Transcode(const AVFrame* in_frame) {

        if (!in_frame) return nullptr;
        if (!sws_ctx) return nullptr;

        // 使用 ACFramePool 分配输出 AVFrame，以保持与项目内存管理一致
        AVFrame* out = ACFramePool::DefaultPool().Allocate();
        if (!out) return nullptr;

        out->format = out_fmt;
        out->width = in_frame->width;
        out->height = in_frame->height;

        // 为目标帧分配数据缓冲区（alignment 为 32）
        if (av_frame_get_buffer(out, 32) < 0) {
            ACFramePool::DefaultPool().Free(out);
            return nullptr;
        }

        // 如果输入帧不是可写或行大小异常，不做修改；直接进行缩放复制
        int ret = sws_scale(sws_ctx,
                            in_frame->data, in_frame->linesize,
                            0, in_frame->height,
                            out->data, out->linesize);
        if (ret <= 0) {
            ACFramePool::DefaultPool().Free(out);
            return nullptr;
        }

        // 拷贝时间信息
        out->pts = in_frame->pts;
        out->duration = in_frame->duration;

        return out;
    }

    AVPixelFormat GetInputFormat() const { return in_fmt; }
    AVPixelFormat GetOutputFormat() const { return out_fmt; }

    ~ACCPUTranscoder() {
        if (sws_ctx) sws_freeContext(sws_ctx);
    }

private:
    // 私有 Reinit：当格式或分辨率改变时调用以（重）建 SWS 上下文
    void Reinit(AVPixelFormat new_in, AVPixelFormat new_out, int w, int h) {
        if (sws_ctx) {
            sws_freeContext(sws_ctx);
            sws_ctx = nullptr;
        }
        in_fmt = new_in;
        out_fmt = new_out;
        width = w;
        height = h;
        sws_ctx = sws_getContext(width, height, in_fmt,
                                 width, height, out_fmt,
                                 SWS_BILINEAR, nullptr, nullptr, nullptr);
    }

    AVPixelFormat in_fmt{ AV_PIX_FMT_NONE };
    AVPixelFormat out_fmt{ AV_PIX_FMT_NONE };
    int width{ 0 };
    int height{ 0 };
    SwsContext* sws_ctx{ nullptr };
};


/**
 * @brief 预分析器
 * 
 * 管理预分析线程，定位视频中的暂停按钮。
*/
class PreAnalyser {
public:
    PreAnalyser() {
        decoder_ = nullptr;
        locator_ = nullptr;
        result_ = {.box = cv::Rect(), .score = 0.0};

        thread_ = std::thread(&PreAnalyser::Analyser, this);
        thread_.detach();
    }

    PreAnalyser(const PreAnalyser&&) = delete;

    /**
     * @brief 输入文件改变时调用
     * 
     * 当输入文件改变时，调用此函数更新预分析器的输入文件路径。
     * 这会使预分析器重新初始化，清空之前的分析结果。
     * 
     * @param new_input_filename 新的输入文件路径
    */
    void OnInputChanged(const Setting& setting) {
        std::lock_guard<std::mutex> lock(onchange_mutex_);
        input_filename_ = setting.input_filename;
        // Decoder 随文件变化，Locator 只初始化一次（因为还没有加入第二种检测模式）
        decoder_.reset(new ACDecoder(input_filename_, ACDecoder::Codec::DXVA2));
        if (!locator_) locator_.reset(new ACLocator(setting.locator_filename));
        // 清空 result
        result_ = {.box = cv::Rect(), .score = 0.0};
        // 跳转到 20 秒
        decoder_->Seek(20);
        // 设置状态
        g_progress_metrics.state = WorkState::sLocating;
    }

    LocatorResult GetResult() const noexcept { return result_; }

    ~PreAnalyser() {
        if (thread_.joinable()) thread_.join();
    }
private:
    std::thread thread_;
    std::mutex onchange_mutex_;

    std::string input_filename_;
    std::unique_ptr<ACDecoder> decoder_;
    std::unique_ptr<ACLocator> locator_;

    LocatorResult result_;

    AVFrame* __Decode() {
        // Sleep here, waken up after OnInputChanged() called
        while (!decoder_ || !locator_) std::this_thread::yield();

        onchange_mutex_.lock();
        AVFrame* frame = decoder_->Decode();
        onchange_mutex_.unlock();
        return frame;
    }

    void Analyser() {
        // 从解码器接收的视频帧
        AVFrame* decoded_frame = nullptr;
        
        // 计时器
        ACMsTimer timer{ DISPLAY_INTERVAL_MS };

        for (int frame_count = 0; (decoded_frame = __Decode()); frame_count++)  {

            auto new_result = locator_->Locate(decoded_frame);

            // 定位阶段进度更新
            g_progress_metrics.progress_percent = float(100.0 * new_result.score * new_result.score);
            g_progress_metrics.frames_per_second = float(frame_count) / timer.Count() * 1000;
            g_progress_metrics.queue_depth = 1;
            g_progress_metrics.eta_seconds = 0;
            
            if (new_result.score < 0.95) continue;
            // 当累计到 1 个检测结果时, 即确定检测坐标
            result_ = new_result;
            // 重置状态
            g_progress_metrics.state = WorkState::sIdle;
            // 清空指针，这样该线程就会休眠
            onchange_mutex_.lock();
            decoder_.reset();
            locator_.reset();
            onchange_mutex_.unlock();
        }
        throw std::runtime_error("无法定位到输入视频中的暂停按钮");
    }
};

/**
* 指定范围的 DetectedFrame 序列中存在 PAUSE 和 SELECT_PAUSE 的切换时, 返回 true.
* 
* 如果不存在, 返回 false.
*
* @param seq 序列
* @param C 当前待提交帧在序列中的位置
*
* @retval bool
*/
bool CheckSwitch(const std::span<DetectedFrame>& seq, size_t C)
{
    // 未选中 PAUSE -> 选中 SELECT_PAUSE
    for (size_t i = 0; i + 1 <= C; i++)
        if (seq[i].state == FrameState::PAUSE) {
            size_t j = i + 1;
            while (j <= C && seq[j].state == FrameState::UNDEFINED)
                j++;
            if (j <= C && seq[j].state == FrameState::SELECT_PAUSE)
                return true;
        }
    // 选中 SELECT_PAUSE -> 未选中 PAUSE
    for (size_t i = 0; i + 1 < seq.size(); i++)
        if (seq[i].state == FrameState::SELECT_PAUSE) {
            size_t j = i + 1;
            while (j < seq.size() && seq[j].state == FrameState::UNDEFINED)
                j++;
            if (j < seq.size() && seq[j].state == FrameState::PAUSE)
                return true;
        }
    return false;
}


/**
* 指定范围的 DetectedFrame 序列中存在 SELECT/SELECT_PAUSE/DEPLOY 到 PLAY 的切换时, 返回 true.
*
* 如果不存在, 返回 false.
*
* @param seq 序列
* @param C 当前待提交帧在序列中的位置
*
* @retval bool
*/
bool CheckPlayingAnimation(const std::span<DetectedFrame>& seq, size_t C)
{
    for (size_t i = 0; i + 1 <= C; i++)
        if (seq[i].state == FrameState::SELECT || 
            seq[i].state == FrameState::SELECT_PAUSE || 
            seq[i].state == FrameState::DEPLOY)
        {
            size_t j = i + 1;
            while (j <= C && seq[j].state == FrameState::UNDEFINED)
                j++;
            if (j <= C && seq[j].state == FrameState::PLAY)
                return true;
        }
    return false;
}

// 启动互斥锁
static std::mutex g_start_mutex;
// 预分析器
static PreAnalyser g_analyser;


void Start(const Setting& setting) {
    
    if (g_start_mutex.try_lock() == false) {
        throw std::runtime_error("重复启动");
        return;
    }

    // 编码器字符串到枚举的映射
    const std::unordered_map<std::string, ACEncoder::Codec> ENCODER_MAP = {
        { "auto", ACEncoder::Codec::AUTO },
        { "cpu", ACEncoder::Codec::OPENH264 },
        { "qsv", ACEncoder::Codec::QSV },
        { "nvenc", ACEncoder::Codec::NVENC },
        { "amf", ACEncoder::Codec::AMF },
    };
    // 解码器字符串到枚举的映射
    const std::unordered_map<std::string, ACDecoder::Codec> DECODER_MAP = {
        { "dxva2", ACDecoder::Codec::DXVA2 },
        { "cpu", ACDecoder::Codec::CPU },
    };
    
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_WARNING);
#ifndef _DEBUG
    av_log_set_level(AV_LOG_FATAL);
#else
    av_log_set_level(AV_LOG_WARNING);
#endif

    ACEncoder::Codec prior_encoder = ACEncoder::Codec::AUTO;
    ACDecoder::Codec prior_decoder = ACDecoder::Codec::DXVA2;
    if (ENCODER_MAP.contains(setting.encoder)) {
        prior_encoder = ENCODER_MAP.at(setting.encoder);
    }
    if (DECODER_MAP.contains(setting.decoder)) {
        prior_decoder = DECODER_MAP.at(setting.decoder);
    }

    // 第一轮：定位暂停按钮
    // 计时器
    ACMsTimer timer{ DISPLAY_INTERVAL_MS };

    LocatorResult detect_result = g_analyser.GetResult();
    while (detect_result.score < 0.95) {
        std::this_thread::yield();
        detect_result = g_analyser.GetResult();
    }

    // 第二轮：识别暂停
    g_progress_metrics.state = WorkState::sClipping;
    ACDecoder input {
        setting.input_filename, prior_decoder
    };
    PixelDetector detector {
        detect_result.box
    };
    ACEncoder output {
        setting.output_filename, &input, prior_encoder, ACEncoder::Format::NV12, setting.output_bitrate > 0 ? setting.output_bitrate : input.GetAvgBitrate()
    };
    ACCPUTranscoder transcoder {
        input.GetFormat(), output.GetFormat(), input.GetWidth(), input.GetHeight()
    };

#ifdef _DEBUG
    std::ofstream debug_file;
    debug_file.open("Detect-Debug.txt", std::ios::out);
#endif
    
    // 当暂停在选中/未选中干员间切换时, 会保留 select_pause_reserved_time (单位: 秒) 的过渡动画不被剪去
    // 该值决定了剪暂停过程中的 "滑动窗口长度", 只有窗口内的帧会在内存中。
    int64_t reserved_pts = (int64_t)ceil(av_q2d(av_inv_q(input.GetTimebase())) * setting.select_pause_reserved_time);

    // 当一倍速下从选中干员切回未选中状态时, 会保留 0.18s 的过渡动画不加速
    int64_t play1x_anime_pts = (int64_t)ceil(av_q2d(av_inv_q(input.GetTimebase())) * 0.18);

    // 视频帧列表
    // 所有解码后的视频帧都会以播放顺序进入该列表中, 视频帧被重新编码后, AVFrame 释放, 其余信息保留。
    std::vector<DetectedFrame> frame_array;
    frame_array.reserve(input.GetFrameCount() + 1);
    // 视频转码标志
    bool transcode_flag = (input.GetFormat() != output.GetFormat());
    // 视频解码结束标志
    bool video_end_flag = false;

    // 下一个将要提交 (判断是否编码) 视频帧在 frame_array 中的索引
    // 一个视频帧可以被提交当且仅当它的 pts + duration 比最新解码视频帧的 pts 小至少 reserved_pts
    size_t next_commit_idx = 0;
    // 根据 pts 快速查询对应帧在 frame_array 中的下标
    std::map<int64_t, size_t> pts_indexing;

    timer.Reset();

    // 当场解码 0 号帧
    AVFrame* decoded_frame = ACFramePool::DefaultPool().Allocate();
    auto ret = input.Decode(decoded_frame);
    if (ret == -1) {
        throw std::runtime_error("无法读取视频帧");
    }
    if (transcode_flag) {
        AVFrame* transcoded_frame = transcoder.Transcode(decoded_frame);
        if (!transcoded_frame) {
            throw std::runtime_error("CPU 转换帧格式失败");
        }
        ACFramePool::DefaultPool().Free(decoded_frame);
        decoded_frame = transcoded_frame;
    }
    frame_array.emplace_back(DetectedFrame{
        .frame    = decoded_frame,
        .pts      = decoded_frame->pts,
        .duration = decoded_frame->duration,
        .state    = detector.Detect(decoded_frame),
    });
    pts_indexing[decoded_frame->pts] = 0;


    for (; next_commit_idx < frame_array.size(); next_commit_idx++) {

        // 当前准备判定的帧
        DetectedFrame& cmt_frame = frame_array[next_commit_idx];

        // 解码新视频帧, 直到满足检测 next_commit_idx 所需的前后区间为止
        while (!video_end_flag && 
               frame_array.back().pts < cmt_frame.pts + cmt_frame.duration + reserved_pts)
        {
            decoded_frame = ACFramePool::DefaultPool().Allocate();
            auto ret = input.Decode(decoded_frame);
            if (ret == -1) {
                video_end_flag = true;
                ACFramePool::DefaultPool().Free(decoded_frame);
                pts_indexing[INT64_MAX] = frame_array.size();       // End flag
                break;
            }
            if (transcode_flag) {
                AVFrame* transcoded_frame = transcoder.Transcode(decoded_frame);
                if (!transcoded_frame) {
                    throw std::runtime_error("CPU 转换帧格式失败");
                }
                ACFramePool::DefaultPool().Free(decoded_frame);
                decoded_frame = transcoded_frame;
            }
            // 规范化前一帧 (frame_array.back().frame) 的持续时间
            frame_array.back().frame->duration =
                std::min(frame_array.back().frame->duration, decoded_frame->pts - frame_array.back().pts);
            // 向 frame_array 插入新帧
            frame_array.emplace_back(DetectedFrame{
                .frame    = decoded_frame,
                .pts      = decoded_frame->pts,
                .duration = decoded_frame->duration,
                .state    = detector.Detect(decoded_frame),
            });
            pts_indexing[decoded_frame->pts] = frame_array.size() - 1;
#ifdef _DEBUG
            auto sec = av_rescale_q(frame_array.back().frame->pts, input.GetTimebase(), AVRational{ 1,1 });
            debug_file << std::format("{:02}:{:02} {}\n", sec / 60, sec % 60, FrameStateStr(frame_array.back().state));
#endif
        }

        // 选中/未选中的切换动画 [ CMT_PTS - R_PTS, CMT_PTS + R_PTS ]
        size_t backward_idx = pts_indexing.lower_bound(cmt_frame.pts - reserved_pts)->second,
               forward_idx  = pts_indexing.upper_bound(cmt_frame.pts + reserved_pts)->second;
        bool reserved = CheckSwitch(
            std::span<DetectedFrame>(frame_array).subspan(backward_idx, forward_idx - backward_idx),
            next_commit_idx - backward_idx
        );
        // 一倍速下视角切回的动画 [ CMT_PTS - PA_PTS, CMT_PTS ]
        size_t play1x_anime_idx = pts_indexing.lower_bound(cmt_frame.pts - play1x_anime_pts)->second;
        bool play1x_animation = CheckPlayingAnimation(
            std::span<DetectedFrame>(frame_array).subspan(play1x_anime_idx, next_commit_idx - play1x_anime_idx + 1),
            next_commit_idx - play1x_anime_idx
        );
        if (reserved || (cmt_frame.state != FrameState::PAUSE && cmt_frame.state != FrameState::SELECT_PAUSE))
        {
            float speed = 1.0f;
            if (cmt_frame.state == FrameState::PLAY && !play1x_animation) speed = setting.acceleration.play1x;
            else if (cmt_frame.state == FrameState::PLAY_2X) speed = setting.acceleration.play2x;
            else if (cmt_frame.state == FrameState::SELECT) speed = setting.acceleration.select;
            else if (cmt_frame.state == FrameState::DEPLOY) speed = setting.acceleration.deploy;
            output.Encode(cmt_frame.frame, speed);
        }

        // 已提交, 释放帧内存
        ACFramePool::DefaultPool().Free(cmt_frame.frame);
        cmt_frame.frame = nullptr;

        // 更新全局进度指标
        g_progress_metrics.progress_percent = 100.0f * frame_array.size() / input.GetFrameCount();
        g_progress_metrics.frames_per_second = frame_array.size() * 1000.0f / timer.Count();
        g_progress_metrics.queue_depth = (int)(frame_array.size() - next_commit_idx);
        g_progress_metrics.eta_seconds = (int)((input.GetFrameCount() - frame_array.size()) / g_progress_metrics.frames_per_second * 1.025f);
    }

#ifdef _DEBUG
    debug_file.close();
#endif

    // 写入文件尾
    output.Close();

    // 处理完成，设置进度为100%
    g_progress_metrics.progress_percent = 100.0f;
    g_progress_metrics.queue_depth = 0;
    g_progress_metrics.eta_seconds = 0;
    g_progress_metrics.state = WorkState::sIdle;
    g_start_mutex.unlock();
}

void OnInputfileChanged(const Setting& setting) {
    g_analyser.OnInputChanged(setting);
}

ProgressMetrics GetProgressMetrics() {
    return g_progress_metrics;
}