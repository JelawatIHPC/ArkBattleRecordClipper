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

	/* 构造函数 (双模板)
	 *
	 * @param detect_region1 第一个定位模板在图像中的范围
	 * @param detect_region2 第二个定位模板在图像中的范围
	 */
	Detector(cv::Rect detect_region1, cv::Rect detect_region2);

	/* 检测当前帧的状态 */
	virtual FrameState Detect(const AVFrame* frame) = 0;

	~Detector();

protected:
	cv::Rect detect_region1;   // 第一个定位模板在图像中的范围
	cv::Rect detect_region2;   // 第二个定位模板在图像中的范围
};

/* 像素法
*/
class PixelDetector : public Detector {
public:
	/* 构造函数 (双模板)
	 *
	 * @param detect_region1 第一个定位模板 (locator.png) 在图像中的范围
	 *                       用于计算检测点 0/3/4
	 * @param detect_region2 第二个定位模板 (locator2.png) 在图像中的范围
	 *                       用于计算检测点 1/2
	 * @param format 输入视频帧的像素格式, 仅支持 NV12 / YUV420P;
	 *               其他格式构造时抛出 std::runtime_error
	 */
	PixelDetector(cv::Rect detect_region1, cv::Rect detect_region2, AVPixelFormat format);

	/* 检测当前帧的状态 */
	FrameState Detect(const AVFrame* frame);

	/* 可视化调试: 将当前帧转换为 BGR 并绘制两个定位区域与 5 个检测点,
	 * 通过 cv::imshow ("PixelDetector") 弹出窗口显示。
	 * 仅为调试设计, 正常流程不调用; 需要排查检测问题时在外部手动调用,
	 * 例如在 PixelDetector::Detect 中临时插入 Visualize(frame)。
	 * 注意: 每帧调用会显著拖慢处理速度。
	 *
	 * @param frame 视频帧, NV12 像素格式
	 */
	void Visualize(const AVFrame* frame) const;

	~PixelDetector();
private:
	std::pair<int, int> detect_points[5];

	/* 输入帧像素格式, 仅 NV12 / YUV420P (构造时校验) */
	AVPixelFormat pixel_format{ AV_PIX_FMT_NONE };

	bool Activated(const AVFrame* frame, std::pair<int, int>& coord);
};

/* 渡空法
*/
class SimilarityDetector : public Detector {
public:
	/* 构造函数 (双模板)
	 *
	 * @param detect_region1 第一个定位模板在图像中的范围
	 * @param detect_region2 第二个定位模板在图像中的范围
	 * @param locator_filename 定位模板图像文件路径
	 */
	SimilarityDetector(cv::Rect detect_region1, cv::Rect detect_region2, const std::string& locator_filename);

	/* 检测当前帧的状态 */
	FrameState Detect(const AVFrame* frame);

	~SimilarityDetector();
private:
	cv::Rect detect_region;
	cv::Mat  locator_gray;
};