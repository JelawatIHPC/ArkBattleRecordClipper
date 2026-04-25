#pragma once

#include <vector>
#include <opencv2/opencv.hpp>

extern "C" {
#include <libavutil/frame.h>
}

// 结构体用于存储匹配结果
struct MatchResult {
	cv::Rect bounding_box;
	double   match_score;
	double   scale;

	/* 计算多个 MatchResult 的平均值 */
	static MatchResult Mean(std::vector<MatchResult>& V);
};

struct LocatorResult {
	cv::Rect box;
	double   score;
};

class ACLocator {
	/* 基于 OpenCV 的暂停按钮定位功能封装
	*/
public:
	ACLocator() = delete;
	ACLocator(const ACLocator&& other) = delete;

	ACLocator(const std::string& locator_filename);
	~ACLocator();

	LocatorResult Locate(const AVFrame* frame, double threshold = 0.95);

private:
	cv::Mat locator_gray;

	std::vector<cv::Mat> roughly_locator_planes;

	LocatorResult RoughlyLocate(const AVFrame* frame, double threshold = 0.95);
};