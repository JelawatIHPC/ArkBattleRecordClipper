#include "ACLocator.h"

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

LocatorResult _locate(const cv::Mat& img, const cv::Mat& templ, double threshold) {
    // 执行模板匹配
    cv::Mat result;
    cv::matchTemplate(img, templ, result, cv::TM_CCOEFF_NORMED);

    // 寻找最佳匹配位置
    double min_val, max_val;
    cv::Point min_loc, max_loc;
    cv::minMaxLoc(result, &min_val, &max_val, &min_loc, &max_loc);

    if (max_val >= threshold) {
        return {cv::Rect(max_loc.x, max_loc.y, templ.cols, templ.rows), max_val};
    } else {
        return {cv::Rect(), max_val}; // No match found
    }
}

ACLocator::ACLocator(const std::string& locator_filename) {
    // 读取暂停按钮图片
    cv::Mat locator_img = cv::imread(locator_filename, cv::IMREAD_COLOR);
    if (locator_img.empty()) {
        throw std::runtime_error("无法读取暂停按钮图像文件");
    }

    // 转换为灰度图像
    cv::cvtColor(locator_img, locator_gray, cv::COLOR_BGR2GRAY);

    roughly_locator_planes.clear();
    if (locator_gray.empty()) {
        throw std::runtime_error("locator_gray is empty");
    }

    // 生成高度为 5..9 的等比例缩放版本
    for (int target_h = 5; target_h <= 9; ++target_h) {
        double scale = (double)target_h / locator_gray.rows;
        int new_w = std::max(1, (int)round(locator_gray.cols * scale));
        cv::Mat resized;
        cv::resize(locator_gray, resized, cv::Size(new_w, target_h), 0, 0, cv::INTER_LINEAR);
        roughly_locator_planes.emplace_back(std::move(resized));
    }
}

ACLocator::~ACLocator() {
}

LocatorResult ACLocator::Locate(const AVFrame* frame, double threshold) {

    LocatorResult rough_result = RoughlyLocate(frame, 0.8);
    if (rough_result.score < 0.8) {
        return LocatorResult{{}, rough_result.score};
    }

    // 提取 NV12 格式 Y 平面创建灰度图像
    cv::Mat y_plane(frame->height, frame->width, CV_8UC1, frame->data[0], frame->linesize[0]);

    // 根据 RoughlyLocate 结果裁剪区域
    cv::Mat y_plane_cropped = y_plane(rough_result.box);

    // 在多尺度上匹配暂停按钮
    LocatorResult best;
    cv::Mat locator_plane;
    int lower_bound = (int)floor(rough_result.box.height * 0.666666666666667);
    int upper_bound = (int) ceil(rough_result.box.height * 0.833333333333333);
    
    for (int h = upper_bound; h >= lower_bound; --h) {
        // 缩放暂停按钮
        double scale = (double)h / locator_gray.rows;
        auto scaled_size = cv::Size((int)round(locator_gray.cols * scale), h);
        cv::resize(locator_gray, locator_plane, scaled_size, 0, 0, cv::INTER_LANCZOS4);

        // 执行模板匹配
        auto result = _locate(y_plane_cropped, locator_plane, threshold);

        // 找到匹配结果
        if (result.score >= threshold && result.score > best.score) {
            best = result;
        }
    }

    best.box.x += rough_result.box.x;
    best.box.y += rough_result.box.y;
    return best;
}

LocatorResult ACLocator::RoughlyLocate(const AVFrame* frame, double threshold) {
    LocatorResult best_result{{}, 0.0};

    // 提取 NV12 格式 Y 平面创建灰度图像
    cv::Mat y_plane(frame->height, frame->width, CV_8UC1, frame->data[0], frame->linesize[0]);
    
    // 等比例缩放到高度 144 px
    cv::Mat scaled_y_plane;
    double scale = 144.0 / y_plane.rows;
    cv::resize(y_plane, scaled_y_plane, cv::Size(), scale, scale, cv::INTER_LANCZOS4);
    
    for (int h = 5; h <= 9; ++h) {
        LocatorResult result = _locate(scaled_y_plane, roughly_locator_planes[h - 5], threshold);
        
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