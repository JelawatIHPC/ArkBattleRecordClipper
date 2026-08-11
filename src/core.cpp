#include <string>
#include <vector>
#include <chrono>
#include <mutex>
#include <future>
#include <atomic>
#include <span>
#include <stdexcept>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include "encoder.h"
#include "decoder.h"
#include "detector.h"
#include "memory.h"
#include "locator.h"
#include "core.h"

#define POW2(x) ((x)*(x))

// 向控制台打印一次进度的最短间隔 (ms)
constexpr int64_t DISPLAY_INTERVAL_MS = 333;

// 全局进度指标存储 (原子快照, 任意线程可安全读写)
static std::atomic<ProgressMetrics> g_progress_metrics;

/* 读取当前进度指标快照
 *
 * @return ProgressMetrics 当前进度指标
 */
static ProgressMetrics MetricsSnapshot() {
    return g_progress_metrics.load();
}

/* 写入进度指标
 *
 * @param metrics 新的进度指标
 */
static void MetricsStore(const ProgressMetrics& metrics) {
    g_progress_metrics.store(metrics);
}

/* 仅更新工作状态, 其余字段保持不变
 *
 * @param state 新的工作状态
 */
static void MetricsSetState(WorkState state) {
    ProgressMetrics metrics = g_progress_metrics.load();
    metrics.state = state;
    g_progress_metrics.store(metrics);
}

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
 * 输入文件改变时, 启动异步分析线程定位视频中的暂停按钮,
 * 分析结果通过 std::future 返回给剪辑流程。
*/
class PreAnalyser {
public:
    PreAnalyser() = default;
    PreAnalyser(const PreAnalyser&& other) = delete;

    /**
     * @brief 输入文件改变时调用
     * 
     * 启动一次新的异步预分析, 并取消之前尚未完成的分析。
     * 
     * @param setting 新的输入文件配置
    */
    void OnInputChanged(const Setting& setting) {
        std::lock_guard<std::mutex> lock(mutex_);
        current_++;
        future_ = std::async(std::launch::async, [this, setting, generation = current_.load()]() {
            return Analyse(setting, generation);
        });
    }

    /**
     * @brief 获取预分析结果
     * 
     * 阻塞等待当前预分析完成并返回定位结果。
     * 
     * @return LocateResult 定位结果
     * 
     * @throw std::runtime_error 预分析失败或未完成时抛出
    */
    LocateResult GetResult() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!future_.valid()) throw std::runtime_error("预分析尚未开始, 请先选择输入文件");
        LocateResult result = future_.get();
        if (result.locator1.box.empty() || result.locator2.box.empty())
            throw std::runtime_error("预分析未完成, 请重新选择输入文件");
        return result;
    }

    ~PreAnalyser() {
        // 取消正在运行的分析, 并等待线程退出
        current_++;
        std::lock_guard<std::mutex> lock(mutex_);
        if (future_.valid()) future_.wait();
    }
private:
    /**
     * @brief 在后台线程中执行预分析
     * 
     * @param setting 输入文件配置
     * @param generation 本次分析的世代号, 检测到世代号过期时放弃本次分析
     * 
     * @return LocateResult 定位结果, 分析被取消时返回两个 box 均为空的结果
    */
    LocateResult Analyse(const Setting& setting, int generation) {
        ProgressMetrics metrics = MetricsSnapshot();
        metrics.state = WorkState::sLocating;
        MetricsStore(metrics);

        ACDecoder decoder(setting.input_filename, ACDecoder::Codec::DXVA2);
        ACLocator locator(setting.locator_filename, setting.locator2_filename);
        decoder.Seek(20);

        // 两个模板各自的有效定位结果集合
        std::vector<LocatorResult> results1, results2;

        ACMsTimer timer{ DISPLAY_INTERVAL_MS };

        for (int frame_count = 0; AVFrame* decoded_frame = decoder.Decode(); frame_count++) {
            // 输入文件已切换, 放弃本次分析
            if (generation != current_.load()) return LocateResult{};

            LocateResult new_result = locator.Locate(decoded_frame);

            // 定位阶段进度更新
            metrics = MetricsSnapshot();
            metrics.progress_percent = 0.0;
            metrics.frames_per_second = float(frame_count) / timer.Count() * 1000;
            metrics.queue_depth = 1;
            metrics.eta_seconds = 0;
            MetricsStore(metrics);

            // 收集有效定位结果 (box 为空说明该帧没有对应模板)
            if (!new_result.locator1.box.empty()) results1.push_back(new_result.locator1);
            if (!new_result.locator2.box.empty()) results2.push_back(new_result.locator2);

            // 两个模板都有至少 1 个有效结果, 求平均减小误差后结束定位阶段
            if (!results1.empty() && !results2.empty()) {
                MetricsSetState(WorkState::sIdle);
                return {
                    LocatorResult::Mean(results1),
                    LocatorResult::Mean(results2)
                };
            }
        }
        throw std::runtime_error("无法定位到输入视频中的暂停按钮");
    }

    std::mutex mutex_;
    std::atomic<int> current_{ 0 };
    std::future<LocateResult> future_;
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

    // 第一轮：等待预分析完成, 获取两个暂停按钮定位结果
    LocateResult detect_result = g_analyser.GetResult();

    // 计时器
    ACMsTimer timer{ DISPLAY_INTERVAL_MS };

    // 第二轮：识别暂停
    MetricsSetState(WorkState::sClipping);
    ACDecoder input {
        setting.input_filename, prior_decoder
    };
    PixelDetector detector {
        detect_result.locator1.box, detect_result.locator2.box
    };
    ACEncoder output {
        setting.output_filename, &input, prior_encoder, ACEncoder::Format::NV12, setting.output_bitrate > 0 ? setting.output_bitrate : input.GetAvgBitrate()
    };
    ACCPUTranscoder transcoder {
        input.GetFormat(), output.GetFormat(), input.GetWidth(), input.GetHeight()
    };

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
        ProgressMetrics metrics = MetricsSnapshot();
        metrics.progress_percent = 100.0f * frame_array.size() / input.GetFrameCount();
        metrics.frames_per_second = frame_array.size() * 1000.0f / timer.Count();
        metrics.queue_depth = (int)(frame_array.size() - next_commit_idx);
        metrics.eta_seconds = (int)((input.GetFrameCount() - frame_array.size()) / metrics.frames_per_second * 1.025f);
        MetricsStore(metrics);
    }

    // 写入文件尾
    output.Close();

    // 处理完成，设置进度为100%
    ProgressMetrics metrics = MetricsSnapshot();
    metrics.progress_percent = 100.0f;
    metrics.queue_depth = 0;
    metrics.eta_seconds = 0;
    metrics.state = WorkState::sIdle;
    MetricsStore(metrics);
    g_start_mutex.unlock();
}

void OnInputfileChanged(const Setting& setting) {
    g_analyser.OnInputChanged(setting);
}

ProgressMetrics GetProgressMetrics() {
    return g_progress_metrics.load();
}