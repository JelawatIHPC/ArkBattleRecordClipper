#include "detector.h"
#include "opencv2/core/types.hpp"
#include <algorithm>
#include <stdexcept>
#include <vector>
#include <cstring>

/* 平方 */
#define POW2(x) ((x)*(x))

Detector::Detector(cv::Rect detect_region1, cv::Rect detect_region2)
    : detect_region1(detect_region1), detect_region2(detect_region2) {
}

Detector::~Detector() {
}

/* 检测 AVFrame 的某坐标点是否为亮白色, 是则视为该点激活。
   暂停检测的基础算法。
   NV12: UV 平面交错存储 (U, V) 成对; YUV420P: U / V 平面分离。
*/
bool PixelDetector::Activated(const AVFrame* frame, std::pair<int, int>& coord) {
    int& x = coord.first;
    int& y = coord.second;
    // Check bounds
    if (x < 0 || x >= frame->width || y < 0 || y >= frame->height) return false;

    // Get Y component
    int y_index = y * frame->linesize[0] + x;
    uint8_t Y = frame->data[0][y_index];

    // Get UV components (shared for 2x2 block)
    int uv_x = x / 2;
    int uv_y = y / 2;
    uint8_t U, V;
    if (pixel_format == AV_PIX_FMT_NV12) {
        int uv_index = uv_y * frame->linesize[1] + uv_x * 2;
        U = frame->data[1][uv_index];
        V = frame->data[1][uv_index + 1];
    }
    else {
        // YUV420P
        U = frame->data[1][uv_y * frame->linesize[1] + uv_x];
        V = frame->data[2][uv_y * frame->linesize[2] + uv_x];
    }

    // Convert YUV to RGB
    int y1192 = 1192 * (Y - 16);
    int uv448 = 448 * (U - 128);
    int uv_128 = 128 * (V - 128);
    int r = (y1192 + uv448) >> 10;              // R = 1.1641Y + 0.4375U
    int g = (y1192 - uv_128 - uv448) >> 10;     // G = 1.1641Y - 0.1250V - 0.4375U
    int b = (y1192 + uv_128) >> 10;             // B = 1.1641Y + 0.1250V

    // Clamp values to [0, 255]
    r = (r < 0) ? 0 : (r > 255) ? 255 : r;
    g = (g < 0) ? 0 : (g > 255) ? 255 : g;
    b = (b < 0) ? 0 : (b > 255) ? 255 : b;

    // See as activated if color distance between (248, 248, 248) [#F8F8F8] and (r, g, b)
    // is less than 15.
    // return (sqrt(POW2(r - 248) + POW2(g - 248) + POW2(b - 248)) < 15.0);
    return (r > 145 && g > 145 && b > 145);
}

PixelDetector::PixelDetector(cv::Rect detect_region1, cv::Rect detect_region2, AVPixelFormat format)
    : Detector(detect_region1, detect_region2), pixel_format(format) {
    
    if (format != AV_PIX_FMT_NV12 && format != AV_PIX_FMT_YUV420P) {
        throw std::runtime_error("Pixel Detector 不支持此像素格式。");
    }

    // 检测点 0/3/4 相对第一个模板 (locator.png) 的矩形区域计算
    double origin_x = detect_region1.x + detect_region1.width / 2.0;
    double origin_y = detect_region1.y + detect_region1.height / 2.0;
    double width = detect_region1.width;
    detect_points[0] = { (int)round(origin_x), (int)round(origin_y) };
    detect_points[3] = { (int)round(origin_x +  0.2333 * width), (int)round(origin_y + -0.2500 * width) };
    detect_points[4] = { (int)round(origin_x +  0.3333 * width), (int)round(origin_y +  0.2500 * width) };

    // 检测点 1/2 相对第二个模板 (locator2.png) 的矩形区域计算
    origin_x = detect_region2.x + detect_region2.width / 2.0;
    origin_y = detect_region2.y + detect_region2.height / 2.0;
    width = detect_region2.width;
    detect_points[1] = { (int)round(origin_x + -0.3333 * width), (int)round(origin_y + -0.0208 * width) };
    detect_points[2] = { (int)round(origin_x +  0.0625 * width), (int)round(origin_y + -0.0208 * width) };
}

PixelDetector::~PixelDetector() {
}

/* 把 NV12 帧 (考虑 linesize 对齐) 拷贝为紧凑布局后转 BGR, 绘制两个定位区域与 5 个检测点。
 */
void PixelDetector::Visualize(const AVFrame* frame) const {
    std::vector<uint8_t> nv12_data((size_t)frame->width * frame->height * 3 / 2);
    for (int row = 0; row < frame->height; ++row)
        memcpy(nv12_data.data() + (size_t)row * frame->width,
               frame->data[0] + (size_t)row * frame->linesize[0], frame->width);
    for (int row = 0; row < frame->height / 2; ++row)
        memcpy(nv12_data.data() + (size_t)frame->width * frame->height + (size_t)row * frame->width,
               frame->data[1] + (size_t)row * frame->linesize[1], frame->width);

    cv::Mat nv12((int)(frame->height * 3 / 2), frame->width, CV_8UC1, nv12_data.data());
    cv::Mat bgr;
    cv::cvtColor(nv12, bgr, cv::COLOR_YUV2BGR_NV12);

    cv::rectangle(bgr, detect_region1, cv::Scalar(0, 255, 0), 1);    // 模板 1: 绿色
    cv::rectangle(bgr, detect_region2, cv::Scalar(255, 0, 0), 1);    // 模板 2: 蓝色
    for (int i = 0; i < 5; ++i) {
        cv::circle(bgr, cv::Point(detect_points[i].first, detect_points[i].second), 1, cv::Scalar(0, 0, 255), 1);   // 检测点: 红色
    }

    // 超出窗口上限 (1440x900) 时等比缩小, 保证整个画面和检测点都可见
    constexpr double MAX_W = 1440.0;
    constexpr double MAX_H = 900.0;
    cv::Mat display = bgr;
    if (bgr.cols > MAX_W || bgr.rows > MAX_H) {
        double scale = std::min(MAX_W / bgr.cols, MAX_H / bgr.rows);
        cv::resize(bgr, display, cv::Size(), scale, scale, cv::INTER_AREA);
    }
    cv::imshow("PixelDetector", display);
    cv::waitKey(1);
}

FrameState PixelDetector::Detect(const AVFrame* frame)
{
    if (frame->format != pixel_format) {
        throw std::runtime_error("视频流中出现了 Pixel Detector 不支持的像素格式。");
    }

    bool D0 = Activated(frame, detect_points[0]),
         D1 = Activated(frame, detect_points[1]),
         D2 = Activated(frame, detect_points[2]),
         D3 = Activated(frame, detect_points[3]),
         D4 = Activated(frame, detect_points[4]);
	
    FrameState state;
    if (D2) {
        if (D3 && D4 && !D0) state = D1 ? FrameState::PLAY_2X : FrameState::PLAY;
        else if (!D3 && !D4 && D0) state = FrameState::PAUSE;
        else state = FrameState::UNDEFINED;
    }
    else if (D1) state = FrameState::UNDEFINED;
    else if (D3 && D4 && !D0)  state = FrameState::SELECT;
    else if (!D3 && !D4 && D0) state = FrameState::SELECT_PAUSE;
    else if (!(D0 || D1 || D2 || D3 || D4)) state = FrameState::DEPLOY;
    else state = FrameState::UNDEFINED;

    return state;
}

SimilarityDetector::SimilarityDetector(cv::Rect detect_region1, cv::Rect detect_region2, const std::string& locator_filename)
    : Detector(detect_region1, detect_region2) {
    // 读取暂停按钮图片
    cv::Mat locator_img = cv::imread(locator_filename, cv::IMREAD_COLOR);
    if (locator_img.empty()) {
        throw std::runtime_error("无法读取暂停按钮图像文件");
    }

    // 转换为灰度图像
    cv::cvtColor(locator_img, locator_gray, cv::COLOR_BGR2GRAY);
}

SimilarityDetector::~SimilarityDetector() {
}

FrameState SimilarityDetector::Detect(const AVFrame* frame) {

    throw std::runtime_error("SimilarityDetector::Detect is not implemented yet");
    return FrameState::UNDEFINED;

    // 提取 NV12 格式 Y 平面创建灰度图像
    cv::Mat y_plane(frame->height, frame->width, CV_8UC1, frame->data[0], frame->linesize[0]);

    // 根据 Detector::detect_region 裁剪区域
    cv::Mat y_plane_cropped = y_plane(detect_region);

    // 执行模板匹配
    cv::Mat result;
    cv::matchTemplate(y_plane_cropped, locator_gray, result, cv::TM_CCOEFF_NORMED);

    // 寻找最佳匹配位置
    double min_val, max_val;
    cv::Point min_loc, max_loc;
    cv::minMaxLoc(result, &min_val, &max_val, &min_loc, &max_loc);

    if (max_val >= 0.95) {
        return FrameState::PAUSE;
    } else {
        return FrameState::PLAY;
    }
}

const std::string FrameStateStr(FrameState s) {
    static constexpr char Status_[][14] = {
        "UNDEFINED",
        "PLAY",           // 游戏 1 倍速运行中
        "PLAY_2X",        // 游戏 2 倍速运行中
        "PAUSE",          // 游戏暂停, 但没有选中任何干员
        "SELECT",         // 选中一个场上或待部署区的干员但没有暂停游戏
        "SELECT_PAUSE",   // 选中一个场上或待部署区的干员时暂停了游戏
        "DEPLOY"          // 正在部署干员
    };
    return Status_[(int)s];
}