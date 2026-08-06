#pragma once

#include <string>
#include <cstdint>

// 工作状态枚举
enum class WorkState {
    sIdle = 0,       // 空闲
    sLocating = 1,   // 定位（Start中的第一轮）
    sClipping = 2    // 剪辑（Start中的第二轮）
};

struct Setting {
    // 输入和输出文件名
    std::string input_filename;
    std::string output_filename;
    std::string locator_filename;
    
    // 输出比特率, 0 表示使用输入视频的平均比特率
    int64_t output_bitrate = 0;
    // 动画保留时间
    float select_pause_reserved_time = 0.3f;

    // 加速倍率
    struct {
        float play1x = 1.0f;
        float play2x = 1.0f;
        float select = 1.0f;
        float deploy = 1.0f;
    } acceleration;

    // 优先使用的编码器 ("auto", "cpu", "qsv", "nvenc", "amf")
    std::string encoder = "auto";
    // 优先使用的解码器 ("dxva2", "cpu")
    std::string decoder = "dxva2";
};

// 进度监控指标结构体
struct ProgressMetrics {
    // 工作状态
    WorkState state = WorkState::sIdle;
    // 百分比进度 (0.0 - 100.0)
    float progress_percent = 0.0f;
    // 每秒处理帧数
    float frames_per_second = 0.0f;
    // 队列深度
    int queue_depth = 0;
    // 预计剩余时间 (秒)
    int eta_seconds = 0;
};


/**
 * @brief 启动处理流程
 * 
 * @param setting 配置参数
 */
void Start(const Setting& setting);


/** @brief 当输入文件改变时，调用该函数通知预分析器
 * 
 * @param setting 配置参数
 */
void OnInputfileChanged(const Setting& setting);


/**
 * @brief 获取当前进度指标
 * 
 * @return ProgressMetrics 当前进度指标
 */
ProgressMetrics GetProgressMetrics();
