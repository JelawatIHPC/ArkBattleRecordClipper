#include "ACDetector.h"
#include "opencv2/core/types.hpp"
#include <stdexcept>

/* 平方 */
#define POW2(x) ((x)*(x))

Detector::Detector(cv::Rect detect_region) {
}

Detector::~Detector() {
}

/* 检测 AVFrame 的某坐标点是否为亮白色, 是则视为该点激活。
   暂停检测的基础算法。
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
    int uv_index = uv_y * frame->linesize[1] + uv_x * 2;
    uint8_t U = frame->data[1][uv_index];
    uint8_t V = frame->data[1][uv_index + 1];

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

PixelDetector::PixelDetector(cv::Rect detect_region) : Detector(detect_region) {
    double origin_x = detect_region.x + detect_region.width / 2.0;
    double origin_y = detect_region.y + detect_region.height / 2.0;
    double width = detect_region.width;
    detect_points[0] = { (int)round(origin_x), (int)round(origin_y) };
    detect_points[1] = { (int)round(origin_x + -2.9778 * width), (int)round(origin_y +  0.3889 * width) };
    detect_points[2] = { (int)round(origin_x + -2.6667 * width), (int)round(origin_y +  0.3889 * width) };
    detect_points[3] = { (int)round(origin_x +  0.2333 * width), (int)round(origin_y + -0.2500 * width) };
    detect_points[4] = { (int)round(origin_x +  0.3333 * width), (int)round(origin_y +  0.2500 * width) };
}

PixelDetector::~PixelDetector() {
}

FrameState PixelDetector::Detect(const AVFrame* frame)
{
    bool D0 = Activated(frame, detect_points[0]),
         D1 = Activated(frame, detect_points[1]),
         D2 = Activated(frame, detect_points[2]),
         D3 = Activated(frame, detect_points[3]),
         D4 = Activated(frame, detect_points[4]);
	
    if (D2) {
        if (D3 && D4 && !D0)  return D1 ? FrameState::PLAY_2X : FrameState::PLAY;
        if (!D3 && !D4 && D0) return FrameState::PAUSE;
        return FrameState::UNDEFINED;
    }
    if (D1) return FrameState::UNDEFINED;
    if (D3 && D4 && !D0)  return FrameState::SELECT;
    if (!D3 && !D4 && D0) return FrameState::SELECT_PAUSE;
    if (!(D0 || D1 || D2 || D3 || D4)) return FrameState::DEPLOY;
    return FrameState::UNDEFINED;
}

SimilarityDetector::SimilarityDetector(cv::Rect detect_region, const std::string& locator_filename) : Detector(detect_region) {
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