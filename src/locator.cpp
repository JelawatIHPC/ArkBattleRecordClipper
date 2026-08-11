#include "locator.h"

#include <algorithm>
#include <cstring>
#include <vector>

auto LOCATOR_BG_COLOR = cv::Scalar(0x31, 0x31, 0x31);

MatchResult MatchResult::Mean(std::vector<MatchResult>& V)
{
    double x = 0, y = 0, w = 0, h = 0, ms = 0, s = 0;

    for (auto& m : V) {
        x += m.bounding_box.x;
        y += m.bounding_box.y;
        w += m.bounding_box.width;
        h += m.bounding_box.height;
        ms += m.match_score;
        s += m.scale;
    }
    x /= V.size(), y /= V.size(), w /= V.size(), h /= V.size();
    ms /= V.size(), s /= V.size();

    return MatchResult(
        cv::Rect((int)round(x), (int)round(y), (int)round(w), (int)round(h)),
        ms, s
    );
}

LocatorResult LocatorResult::Mean(std::vector<LocatorResult>& V)
{
    double x = 0, y = 0, w = 0, h = 0, s = 0;

    for (auto& m : V) {
        x += m.box.x;
        y += m.box.y;
        w += m.box.width;
        h += m.box.height;
        s += m.score;
    }
    x /= V.size(), y /= V.size(), w /= V.size(), h /= V.size(), s /= V.size();

    return LocatorResult{
        cv::Rect((int)round(x), (int)round(y), (int)round(w), (int)round(h)),
        s
    };
}

LocatorResult _locate(const cv::Mat& img, const cv::Mat& templ, double threshold) {
    // 执行模板匹配
    cv::Mat result;
    cv::matchTemplate(img, templ, result, cv::TM_CCOEFF_NORMED);

    // 寻找最佳匹配位置
    double min_val = 0.0, max_val = 0.0;
    cv::Point min_loc, max_loc;
    cv::minMaxLoc(result, &min_val, &max_val, &min_loc, &max_loc);

    if (max_val >= threshold) {
        return {cv::Rect(max_loc.x, max_loc.y, templ.cols, templ.rows), max_val};
    } else {
        return {cv::Rect(), max_val}; // No match found
    }
}

ACLocator::ACLocator(const std::string& locator1_filename, const std::string& locator2_filename) {
    const std::string filenames[2] = { locator1_filename, locator2_filename };
    for (const auto& filename : filenames) {
        // 读取模板图片
        cv::Mat locator_img = cv::imread(filename, cv::IMREAD_COLOR);
        if (locator_img.empty()) {
            throw std::runtime_error("无法读取定位模板图像文件: " + filename);
        }

        // 转换为灰度图像
        cv::Mat locator_gray;
        cv::cvtColor(locator_img, locator_gray, cv::COLOR_BGR2GRAY);
        if (locator_gray.empty()) {
            throw std::runtime_error("locator_gray is empty");
        }

        // 生成粗定位平面: 高度范围覆盖 720P (720) ~ 4K (2160) 的输入视频。
        // 帧被等比例缩放到高度 144, 因此模板在缩放帧中的高度为 rows * 144 / frame_height。
        // 高度下限固定为 5: 更小的平面区分度不足, 误判率过高
        int min_target_h = std::max(5, (int)ceil(locator_gray.rows * 144.0 / 2160.0));   // 4K 下最小, 下限 5
        int max_target_h = std::max(min_target_h, (int)round(locator_gray.rows * 144.0 / 720.0));   // 720P 下最大
        std::vector<cv::Mat> roughly_locator_planes;
        for (int target_h = min_target_h; target_h <= max_target_h; ++target_h) {
            double scale = (double)target_h / locator_gray.rows;
            int new_w = std::max(1, (int)round(locator_gray.cols * scale));
            cv::Mat resized;
            // INTER_AREA 均值采样: 缩小后的模板保留整体亮度分布, 小尺寸下区分度优于 INTER_LINEAR
            cv::resize(locator_gray, resized, cv::Size(new_w, target_h), 0, 0, cv::INTER_AREA);
            roughly_locator_planes.emplace_back(std::move(resized));
        }
        templates_.push_back({ locator_gray, std::move(roughly_locator_planes), min_target_h, max_target_h });
    }
}

ACLocator::~ACLocator() {
}

LocateResult ACLocator::Locate(const AVFrame* frame, double threshold) {
    constexpr double ROUGH_THRESHOLD = 0.7;

    // 粗定位阶段: 只寻找模板 1
    LocatorResult rough = RoughlyLocate(frame, 0, ROUGH_THRESHOLD);

    // 精定位阶段: 模板 1 成功后再精定位模板 2
    LocatorResult fine1, fine2;
    if (rough.score >= ROUGH_THRESHOLD) {
        fine1 = LocateFine(frame, 0, rough, threshold);
        if (!fine1.box.empty()) {
            fine2 = LocateFine(frame, 1, fine1, threshold);
        }
    }

    return {
        fine1,
        fine2
    };
}

/* 把 NV12 帧 (考虑 linesize 对齐) 拷贝为紧凑布局后转 BGR, 绘制粗定位/精定位框。
 */
void ACLocator::Visualize(const AVFrame* frame, const LocatorResult& rough,
                          const LocatorResult& fine1, const LocatorResult& fine2) const {
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

    // 垂直中线 (模板 2 搜索区域的左边界)
    cv::line(bgr, cv::Point(frame->width / 2, 0), cv::Point(frame->width / 2, frame->height),
             cv::Scalar(255, 255, 0), 1);
    // 粗定位框: 绿色
    cv::rectangle(bgr, rough.box, cv::Scalar(0, 255, 0), 1);
    // 模板 1 精定位框: 蓝色
    cv::rectangle(bgr, fine1.box, cv::Scalar(255, 0, 0), 1);
    // 模板 2 精定位框: 红色
    cv::rectangle(bgr, fine2.box, cv::Scalar(0, 0, 255), 1);

    // 超出窗口上限 (1440x900) 时等比缩小, 保证整个画面和框都可见
    constexpr double MAX_W = 1440.0;
    constexpr double MAX_H = 900.0;
    cv::Mat display = bgr;
    if (bgr.cols > MAX_W || bgr.rows > MAX_H) {
        double scale = std::min(MAX_W / bgr.cols, MAX_H / bgr.rows);
        cv::resize(bgr, display, cv::Size(), scale, scale, cv::INTER_AREA);
    }
    cv::imshow("ACLocator", display);
    cv::waitKey(1);
}

LocatorResult ACLocator::LocateFine(const AVFrame* frame, size_t index, const LocatorResult& reference, double threshold) {
    const LocatorTemplate& templ = templates_[index];

    // 参考结果 (模板 1 的粗/精定位结果) 为空时直接跳过
    if (reference.box.empty()) {
        return LocatorResult{{}, reference.score};
    }

    // 提取 NV12 格式 Y 平面创建灰度图像
    cv::Mat y_plane(frame->height, frame->width, CV_8UC1, frame->data[0], frame->linesize[0]);

    cv::Mat y_plane_cropped;
    int offset_x = 0, offset_y = 0;
    int lower_bound, upper_bound;

    if (index == 0) {
        // 模板 1: 在粗定位框内搜索, 粗定位框元素贴近画面边缘时会越界, 先裁剪到帧范围内
        cv::Rect box = reference.box & cv::Rect(0, 0, frame->width, frame->height);
        if (box.empty()) {
            return LocatorResult{{}, reference.score};
        }
        y_plane_cropped = y_plane(box);
        offset_x = box.x;
        offset_y = box.y;
        lower_bound = std::max(1, (int)floor(box.height * 0.666666666666667));
        upper_bound = std::max(lower_bound, (int)ceil(box.height * 0.833333333333333));
    } else {
        // 模板 2: 搜索区域为 [垂直中线, 模板 1 左边缘] x [模板 1 上边缘, 模板 1 下边缘+模板 1 高度]。
        // 模板 2 实际位于模板 1 的左下方, 垂直范围向下延伸一个模板 1 高度,
        // 以保证模板 2 即使整体位于模板 1 下缘之下也能完整落入搜索区
        cv::Rect region(frame->width / 2, reference.box.y,
                        reference.box.x - frame->width / 2,
                        reference.box.height * 2);
        cv::Rect box = region & cv::Rect(0, 0, frame->width, frame->height);
        if (box.empty()) {
            return LocatorResult{{}, reference.score};
        }
        y_plane_cropped = y_plane(box);
        offset_x = box.x;
        offset_y = box.y;
        // 精确比例高度: 模板 1 精定位高度存在约 5% 的系统性偏差, 高度窗口放宽到 ±3px
        double height_ratio = (double)templ.locator_gray.rows / templates_[0].locator_gray.rows;
        int base_h = (int)round(reference.box.height * height_ratio);
        lower_bound = std::max(1, base_h - 3);
        upper_bound = std::max(lower_bound, base_h + 3);
    }

    // 模板高度上限: 高度本身与按宽高比换算的宽度都不能超过搜索区域
    int max_h = std::max(1, std::min(y_plane_cropped.rows,
        (int)floor((double)y_plane_cropped.cols * templ.locator_gray.rows / templ.locator_gray.cols)));
    lower_bound = std::min(lower_bound, max_h);
    upper_bound = std::min(upper_bound, max_h);
    if (upper_bound < lower_bound) {
        upper_bound = lower_bound;
    }

    // 在多尺度上匹配模板
    LocatorResult best;
    cv::Mat locator_plane;
    for (int h = upper_bound; h >= lower_bound; --h) {
        // 缩放模板
        double scale = (double)h / templ.locator_gray.rows;
        auto scaled_size = cv::Size((int)round(templ.locator_gray.cols * scale), h);
        cv::resize(templ.locator_gray, locator_plane, scaled_size, 0, 0, cv::INTER_LANCZOS4);

        // 执行模板匹配
        auto result = _locate(y_plane_cropped, locator_plane, threshold);

        // 找到匹配结果
        if (result.score >= threshold && result.score > best.score) {
            best = result;
        }
    }

    // 将搜索区域的偏移加回
    best.box.x += offset_x;
    best.box.y += offset_y;

    return best;
}

LocatorResult ACLocator::RoughlyLocate(const AVFrame* frame, size_t index, double threshold) {
    const LocatorTemplate& templ = templates_[index];
    LocatorResult best_result{{}, 0.0};

    // 提取 NV12 格式 Y 平面创建灰度图像
    cv::Mat y_plane(frame->height, frame->width, CV_8UC1, frame->data[0], frame->linesize[0]);
    
    // 等比例缩放到高度 144 px
    cv::Mat scaled_y_plane;
    double scale = 144.0 / y_plane.rows;
    cv::resize(y_plane, scaled_y_plane, cv::Size(), scale, scale, cv::INTER_LANCZOS4);

    // UI 随视频分辨率等比缩放, 模板按 1080P 设计, 因此在 144 高缩放帧中的
    // 实际高度恒为 rows * 144 / 1080。以 K 为中心取 ±2, 再与模板高度范围求交集
    int center = (int)round((double)templ.locator_gray.rows * 144.0 / 1080.0);
    int h_min = std::max(templ.min_target_h, center - 2);
    int h_max = std::min(templ.max_target_h, center + 2);

    for (int h = h_min; h <= h_max; ++h) {
        size_t i = h - templ.min_target_h;
        LocatorResult result = _locate(scaled_y_plane, templ.roughly_locator_planes[i], threshold);
        
        if (result.score > best_result.score)
            best_result = result;
    }

    // 放大 best_result.box 并加边缘
    double scale_factor = (double)frame->height / 144.0;
    best_result.box.x = (int)round((best_result.box.x - 1) * scale_factor);
    best_result.box.y = (int)round((best_result.box.y - 1) * scale_factor);
    best_result.box.width = (int)round((best_result.box.width + 2) * scale_factor);
    best_result.box.height = (int)round((best_result.box.height + 2) * scale_factor);

    return best_result;
}