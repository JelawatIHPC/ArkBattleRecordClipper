#pragma once

#include <string>
#include <opencv2/opencv.hpp>

extern "C" {
#include <libavutil/frame.h>
}

/* 帧状态 */
enum class FrameState {
	UNDEFINED,
	PLAY,           // 游戏 1 倍速运行中
	PLAY_2X,        // 游戏 2 倍速运行中
	PAUSE,          // 游戏暂停, 但没有选中任何干员
	SELECT,         // 选中一个场上或待部署区的干员但没有暂停游戏
	SELECT_PAUSE,   // 选中一个场上或待部署区的干员时暂停了游戏
	DEPLOY          // 正在部署干员
};

/* 帧状态 -> 名称 */
const std::string FrameStateStr(FrameState s);

class Detector {
public:
	Detector() = delete;
	Detector(const Detector&& other) = delete;

	Detector(cv::Rect detect_region);

	/* 检测当前帧的状态 */
	virtual FrameState Detect(const AVFrame* frame) = 0;

	~Detector();
};

/* 像素法
*/
class PixelDetector : public Detector {
public:
	PixelDetector(cv::Rect detect_region);

	/* 检测当前帧的状态 */
	FrameState Detect(const AVFrame* frame);

	~PixelDetector();
private:
	std::pair<int, int> detect_points[5];

	bool Activated(const AVFrame* frame, std::pair<int, int>& coord);
};

/* 渡空法
*/
class SimilarityDetector : public Detector {
public:
	SimilarityDetector(cv::Rect detect_region, const std::string& locator_filename);

	/* 检测当前帧的状态 */
	FrameState Detect(const AVFrame* frame);

	~SimilarityDetector();
private:
	cv::Rect detect_region;
	cv::Mat  locator_gray;
};