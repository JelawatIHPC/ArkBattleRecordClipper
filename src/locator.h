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

	/* 计算多个 LocatorResult 的平均值, 减小定位误差
	 *
	 * @param V 待平均的结果集合, 不能为空
	 *
	 * @return 平均后的 LocatorResult
	 */
	static LocatorResult Mean(std::vector<LocatorResult>& V);
};

/* 双模板定位结果
 */
struct LocateResult {
	LocatorResult locator1;   // 第一个模板 (locator.png) 的定位结果, box 为空表示该帧未找到
	LocatorResult locator2;   // 第二个模板 (locator2.png) 的定位结果, box 为空表示该帧未找到
};

class ACLocator {
	/* 基于 OpenCV 的双模板定位功能封装
	*/
public:
	ACLocator() = delete;
	ACLocator(const ACLocator&& other) = delete;

	/* 构造函数
	 *
	 * @param locator1_filename 第一个定位模板图像文件路径
	 * @param locator2_filename 第二个定位模板图像文件路径
	 */
	ACLocator(const std::string& locator1_filename, const std::string& locator2_filename);
	~ACLocator();

	/* 在视频帧中定位两个模板
	 *
	 * 先只对模板 1 做粗定位 (阈值 0.8), 粗定位成功后再精定位模板 1;
	 * 模板 1 精确匹配失败时模板 2 也直接跳过, 成功时才以模板 1 的
	 * 精定位结果为参考精定位模板 2。
	 *
	 * @param frame 视频帧, NV12 像素格式
	 * @param threshold 精确匹配阶段的最低匹配分数
	 *
	 * @return LocateResult 两个模板各自的定位结果, 某个 box 为空表示该帧没有对应的模板
	 */
	LocateResult Locate(const AVFrame* frame, double threshold = 0.95);

	/* 小窗口定位: 以最近一次模板 1 的成功结果为中心, 在很小的空间与尺度范围内定位两个模板
	 *
	 * 与 Locate 的全窗口流程 (粗定位 + 精定位) 不同, 本方法直接复用上一帧模板 1 的
	 * 精定位结果: 搜索区域为上一帧框外扩 6px, 高度窗口为上一帧高度 ±2px。
	 * 从未成功定位过模板 1 (内部参考为空) 时直接返回两个空 box。
	 * 模板 1 命中时更新内部参考, 并照常精定位模板 2; 未命中时内部参考保持不变。
	 *
	 * @param frame 视频帧, NV12 像素格式
	 * @param threshold 精确匹配阶段的最低匹配分数
	 *
	 * @return LocateResult 两个模板各自的定位结果, 某个 box 为空表示该帧没有对应的模板
	 */
	LocateResult LocateSmall(const AVFrame* frame, double threshold = 0.95);

private:
	/* 单个模板的模板数据与粗略匹配平面
	 */
	struct LocatorTemplate {
		cv::Mat locator_gray;
		std::vector<cv::Mat> roughly_locator_planes;   // 高度从 min_target_h 起依次递增 1
		int min_target_h;   // 粗定位平面高度下限 (4K 下模板高度, 最小 5)
		int max_target_h;   // 粗定位平面高度上限 (720P 下模板高度)
	};

	std::vector<LocatorTemplate> templates_;   // 长度为 2, 依次对应两个模板

	LocatorResult prev_fine1_;   // 最近一次模板 1 精定位结果, box 为空表示从未成功定位

	/* 可视化调试: 将当前帧转换为 BGR, 绘制粗定位框 (绿色) 与模板 1/2 精定位框 (蓝/红),
	 * 通过 cv::imshow ("ACLocator") 弹出窗口显示。
	 * 仅为调试设计, 正常流程不调用; 排查定位问题时在 Locate 中临时插入调用,
	 * 例如 Visualize(frame, rough, fine1, fine2)。
	 * 注意: 每帧调用会显著拖慢处理速度。
	 *
	 * @param frame 视频帧, NV12 像素格式
	 * @param rough 模板 1 的粗定位结果
	 * @param fine1 模板 1 的精定位结果
	 * @param fine2 模板 2 的精定位结果
	 */
	void Visualize(const AVFrame* frame, const LocatorResult& rough,
	               const LocatorResult& fine1, const LocatorResult& fine2) const;

	/* 精定位单个模板
	 *
	 * 模板 1: 在参考 (粗定位) 框内搜索, 高度范围为框高的 [0.667, 0.833]。
	 * 模板 2: 横向为模板 1 左边缘向左 1~5 倍模板 1 高度, 垂直方向自模板 1
	 * 上边缘向下延伸 2 倍模板 1 高度, 在此区域内搜索;
	 * 高度为模板 1 匹配高度按模板高度比例换算后取 ±3px。
	 *
	 * @param frame 视频帧, NV12 像素格式
	 * @param index 模板索引 (0 或 1)
	 * @param reference 模板 1 的粗定位结果 (index 为 0 时) 或精定位结果 (index 为 1 时), box 为空时直接返回空结果
	 * @param threshold 精确匹配阶段的最低匹配分数
	 *
	 * @return LocatorResult 精定位结果, box 为空表示未找到
	 */
	LocatorResult LocateFine(const AVFrame* frame, size_t index, const LocatorResult& reference, double threshold = 0.95);

	/* 粗糙定位单个模板
	 *
	 * 只尝试 [K-2, K+2] 与模板高度范围 [min_target_h, max_target_h] 交集内的高度,
	 * 其中 K 为模板在 144 高缩放帧中的实际高度 (rows * 144 / 1080,
	 * UI 随视频分辨率等比缩放, 模板按 1080P 设计)。
	 * 例: 模板 1 的 K = 6, 高度范围 [5, 9], 则实际尝试 [5, 8]。
	 *
	 * @param frame 视频帧, NV12 像素格式
	 * @param index 模板索引 (0 或 1)
	 * @param threshold 粗糙匹配阶段的最低匹配分数
	 *
	 * @return LocatorResult 粗糙定位结果, 分数低于 threshold 时 box 为空
	 */
	LocatorResult RoughlyLocate(const AVFrame* frame, size_t index, double threshold = 0.8);
};