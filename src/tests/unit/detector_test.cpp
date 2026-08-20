#include <cstring>
#include <stdexcept>

#include <gtest/gtest.h>

#include "detector.h"

namespace {

constexpr int kWidth = 1280;
constexpr int kHeight = 720;

// 与真实 UI 传参一致的定位区域 (locator.png 与 locator2.png 的检测范围)
const cv::Rect kRegion1(100, 100, 400, 400);
const cv::Rect kRegion2(700, 100, 300, 300);

/* 检测点坐标 (由 PixelDetector 构造公式计算, 改动构造公式需同步更新):
 * p0=(300,300) p1=(750,244) p2=(869,244) p3=(393,200) p4=(433,400)
 */
constexpr std::pair<int, int> kP0(300, 300);
constexpr std::pair<int, int> kP1(750, 244);
constexpr std::pair<int, int> kP2(869, 244);
constexpr std::pair<int, int> kP3(393, 200);
constexpr std::pair<int, int> kP4(433, 400);

/* 创建指定尺寸的 NV12 帧: Y 平面填充 y_value, UV 平面填充 128 (中性色)
 */
AVFrame* CreateNv12Frame(int width, int height, uint8_t y_value) {
  AVFrame* frame = av_frame_alloc();
  frame->format = AV_PIX_FMT_NV12;
  frame->width = width;
  frame->height = height;
  frame->linesize[0] = width;
  frame->linesize[1] = width;
  frame->data[0] = static_cast<uint8_t*>(av_malloc(static_cast<size_t>(width) * height));
  frame->data[1] = static_cast<uint8_t*>(av_malloc(static_cast<size_t>(width) * height / 2));
  std::memset(frame->data[0], y_value, static_cast<size_t>(width) * height);
  std::memset(frame->data[1], 128, static_cast<size_t>(width) * height / 2);
  return frame;
}

void FreeNv12Frame(AVFrame* frame) {
  av_free(frame->data[0]);
  av_free(frame->data[1]);
  av_frame_free(&frame);
}

/* 将帧内某像素的 Y 分量设为指定值 (UV 保持 128), 用于点亮/熄灭检测点
 */
void SetPixelY(AVFrame* frame, int x, int y, uint8_t value) {
  frame->data[0][y * frame->linesize[0] + x] = value;
}

void LightUp(AVFrame* frame, const std::pair<int, int>& point) {
  SetPixelY(frame, point.first, point.second, 255);
}

}  // namespace

/* 构造: 不支持的像素格式抛出异常
 */
TEST(PixelDetector, UnsupportedFormatThrows) {
  EXPECT_THROW(PixelDetector detector(kRegion1, kRegion2, AV_PIX_FMT_YUV444P),
               std::runtime_error);
}

/* 检测: 全黑帧 (无任何激活点) 判定为 DEPLOY
 */
TEST(PixelDetector, DetectAllBlackFrameIsDeploy) {
  PixelDetector detector(kRegion1, kRegion2, AV_PIX_FMT_NV12);
  AVFrame* frame = CreateNv12Frame(kWidth, kHeight, 16);
  EXPECT_EQ(FrameState::DEPLOY, detector.Detect(frame));
  FreeNv12Frame(frame);
}

/* 检测: 仅点亮 p0 与 p2 判定为 PAUSE
 */
TEST(PixelDetector, DetectPauseFrame) {
  PixelDetector detector(kRegion1, kRegion2, AV_PIX_FMT_NV12);
  AVFrame* frame = CreateNv12Frame(kWidth, kHeight, 16);
  LightUp(frame, kP0);
  LightUp(frame, kP2);
  EXPECT_EQ(FrameState::PAUSE, detector.Detect(frame));
  FreeNv12Frame(frame);
}

/* 检测: 点亮 p2/p3/p4 判定为 PLAY
 */
TEST(PixelDetector, DetectPlayFrame) {
  PixelDetector detector(kRegion1, kRegion2, AV_PIX_FMT_NV12);
  AVFrame* frame = CreateNv12Frame(kWidth, kHeight, 16);
  LightUp(frame, kP2);
  LightUp(frame, kP3);
  LightUp(frame, kP4);
  EXPECT_EQ(FrameState::PLAY, detector.Detect(frame));
  FreeNv12Frame(frame);
}

/* 检测: 点亮 p1/p2/p3/p4 判定为 PLAY_2X
 */
TEST(PixelDetector, DetectPlay2xFrame) {
  PixelDetector detector(kRegion1, kRegion2, AV_PIX_FMT_NV12);
  AVFrame* frame = CreateNv12Frame(kWidth, kHeight, 16);
  LightUp(frame, kP1);
  LightUp(frame, kP2);
  LightUp(frame, kP3);
  LightUp(frame, kP4);
  EXPECT_EQ(FrameState::PLAY_2X, detector.Detect(frame));
  FreeNv12Frame(frame);
}

/* 检测: 仅点亮 p3/p4 判定为 SELECT
 */
TEST(PixelDetector, DetectSelectFrame) {
  PixelDetector detector(kRegion1, kRegion2, AV_PIX_FMT_NV12);
  AVFrame* frame = CreateNv12Frame(kWidth, kHeight, 16);
  LightUp(frame, kP3);
  LightUp(frame, kP4);
  EXPECT_EQ(FrameState::SELECT, detector.Detect(frame));
  FreeNv12Frame(frame);
}

/* 检测: 仅点亮 p0 判定为 SELECT_PAUSE
 */
TEST(PixelDetector, DetectSelectPauseFrame) {
  PixelDetector detector(kRegion1, kRegion2, AV_PIX_FMT_NV12);
  AVFrame* frame = CreateNv12Frame(kWidth, kHeight, 16);
  LightUp(frame, kP0);
  EXPECT_EQ(FrameState::SELECT_PAUSE, detector.Detect(frame));
  FreeNv12Frame(frame);
}

/* 检测: 仅点亮 p1 时状态无法确定
 */
TEST(PixelDetector, DetectSinglePoint1IsUndefined) {
  PixelDetector detector(kRegion1, kRegion2, AV_PIX_FMT_NV12);
  AVFrame* frame = CreateNv12Frame(kWidth, kHeight, 16);
  LightUp(frame, kP1);
  EXPECT_EQ(FrameState::UNDEFINED, detector.Detect(frame));
  FreeNv12Frame(frame);
}

/* 检测: 无法归入任何已知组合的状态
 */
TEST(PixelDetector, DetectMixedPatternIsUndefined) {
  PixelDetector detector(kRegion1, kRegion2, AV_PIX_FMT_NV12);
  AVFrame* frame = CreateNv12Frame(kWidth, kHeight, 16);
  LightUp(frame, kP0);
  LightUp(frame, kP3);
  EXPECT_EQ(FrameState::UNDEFINED, detector.Detect(frame));
  FreeNv12Frame(frame);
}

/* 检测: 帧尺寸小于定位区域时所有检测点越界, 判定为 DEPLOY
 */
TEST(PixelDetector, DetectFrameSmallerThanRegionsIsDeploy) {
  PixelDetector detector(kRegion1, kRegion2, AV_PIX_FMT_NV12);
  AVFrame* frame = CreateNv12Frame(320, 240, 16);
  EXPECT_EQ(FrameState::DEPLOY, detector.Detect(frame));
  FreeNv12Frame(frame);
}

/* 帧状态名称: 全部状态可转换为名称
 */
TEST(FrameStateStr, AllStatesHaveNames) {
  EXPECT_EQ("UNDEFINED", FrameStateStr(FrameState::UNDEFINED));
  EXPECT_EQ("PLAY", FrameStateStr(FrameState::PLAY));
  EXPECT_EQ("PLAY_2X", FrameStateStr(FrameState::PLAY_2X));
  EXPECT_EQ("PAUSE", FrameStateStr(FrameState::PAUSE));
  EXPECT_EQ("SELECT", FrameStateStr(FrameState::SELECT));
  EXPECT_EQ("SELECT_PAUSE", FrameStateStr(FrameState::SELECT_PAUSE));
  EXPECT_EQ("DEPLOY", FrameStateStr(FrameState::DEPLOY));
}

/* 相似度检测器: 定位模板文件不存在时抛出异常
 */
TEST(SimilarityDetector, MissingLocatorFileThrows) {
  EXPECT_THROW(SimilarityDetector detector(kRegion1, kRegion2,
                                           "nonexistent_locator.png"),
               std::runtime_error);
}
