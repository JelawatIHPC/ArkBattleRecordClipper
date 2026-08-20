#include <stdexcept>

#include <windows.h>

#include <gtest/gtest.h>

#include "settings.h"

namespace {

/* ANSI 字符串按系统代码页转回 UTF-16 (与实现对应的回转换, 用于编码一致性校验)
 *
 * @param ansi ANSI 系统代码页编码的字符串
 * @return UTF-16 宽字符字符串, 转换失败时为空串
 */
std::wstring AnsiToWide(const std::string& ansi) {
  if (ansi.empty()) {
    return {};
  }
  int len = MultiByteToWideChar(CP_ACP, 0, ansi.c_str(), (int)ansi.size(), nullptr, 0);
  if (len <= 0) {
    return {};
  }
  std::wstring wide((size_t)len, L'\0');
  MultiByteToWideChar(CP_ACP, 0, ansi.c_str(), (int)ansi.size(), wide.data(), len);
  return wide;
}

/* 用 WC_NO_BEST_FIT_CHARS 逐字符探测,
 * 判断宽字符串能否被当前 ANSI 代码页无损表示
 *
 * @param wide UTF-16 宽字符字符串
 * @return 全部字符均可精确编码时为 true
 */
bool CanEncodeInAcp(const std::wstring& wide) {
  for (wchar_t wc : wide) {
    char buf[4] = {};
    int r = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, &wc, 1, buf,
                                sizeof(buf), nullptr, nullptr);
    if (r <= 0) {
      return false;
    }
  }
  return true;
}

}  // namespace

/* 设置解析: 全部字段正确解析, 输入/输出为 UTF-8, 定位模板为系统代码页 (中文系统即 GBK)
 */
TEST(ParseSetting, ParsesAllFields) {
  const std::string req =
      "[{\"input_file\":\"input.mp4\",\"output_file\":\"output.mp4\","
      "\"bitrate\":6000,\"speed_1x\":true,\"bullet_time\":2.5,"
      "\"anim_reserved\":0.5,\"encoder\":\"nvenc\",\"decoder\":\"cpu\"}]";

  Setting setting = ParseSetting(req, L"C:/base");

  EXPECT_EQ("input.mp4", setting.input_filename_utf8);
  EXPECT_EQ("C:/base/output.mp4", setting.output_filename_utf8);
  EXPECT_EQ(6000, setting.output_bitrate);
  EXPECT_FLOAT_EQ(2.0f, setting.acceleration.play1x);
  EXPECT_FLOAT_EQ(2.5f, setting.acceleration.select);
  EXPECT_FLOAT_EQ(0.5f, setting.select_pause_reserved_time);
  EXPECT_EQ("nvenc", setting.encoder);
  EXPECT_EQ("cpu", setting.decoder);
  EXPECT_EQ("C:/base/assets/locator.png", setting.locator_filename_ansi);
  EXPECT_EQ("C:/base/assets/locator2.png", setting.locator2_filename_ansi);
}

/* 设置解析: 空对象使用全部默认值
 */
TEST(ParseSetting, AppliesDefaultsForEmptyObject) {
  Setting setting = ParseSetting("[{}]", L"C:/base");

  EXPECT_TRUE(setting.input_filename_utf8.empty());
  EXPECT_EQ("C:/base/", setting.output_filename_utf8);
  EXPECT_EQ(0, setting.output_bitrate);
  EXPECT_FLOAT_EQ(1.0f, setting.acceleration.play1x);
  EXPECT_FLOAT_EQ(1.0f, setting.acceleration.select);
  EXPECT_FLOAT_EQ(0.3f, setting.select_pause_reserved_time);
  EXPECT_EQ("auto", setting.encoder);
  EXPECT_EQ("dxva2", setting.decoder);
}

/* 设置解析: speed_1x 为 false 时一倍速不加倍
 */
TEST(ParseSetting, Speed1xFalseKeepsPlay1x) {
  Setting setting = ParseSetting("[{\"speed_1x\":false}]", L"C:/base");
  EXPECT_FLOAT_EQ(1.0f, setting.acceleration.play1x);
}

/* 设置解析: 非法 JSON 抛出异常
 */
TEST(ParseSetting, InvalidJsonThrows) {
  EXPECT_THROW(ParseSetting("not-a-json", L"C:/base"), std::runtime_error);
}

/* 设置解析: JSON 缺少外层数组抛出异常
 */
TEST(ParseSetting, MissingArrayLayerThrows) {
  EXPECT_THROW(ParseSetting("{}", L"C:/base"), std::runtime_error);
}

/* 设置解析: 中文基准目录按消费方字符集分发
 * (回归: 曾因定位模板路径以 UTF-8 字节传给 ANSI 消费方,
 *  导致中文系统 (GBK) 下无法读取 locator.png)
 */
TEST(ParseSetting, ChineseBaseDirUsesPerConsumerCharsets) {
  const std::string req =
      "[{\"input_file\":\"录屏.mp4\",\"output_file\":\"剪辑.mp4\"}]";

  Setting setting = ParseSetting(req, L"C:/基地");

  // FFmpeg 消费方: 输入/输出必须为严格 UTF-8
  EXPECT_EQ("录屏.mp4", setting.input_filename_utf8);
  EXPECT_EQ("C:/基地/剪辑.mp4", setting.output_filename_utf8);

  // 定位模板: 仅当期望值能被当前 ANSI 代码页无损表示时才做回转换一致性断言
  // (无法表示时实现按设计退化为 '?' 占位, 不影响 ASCII 路径)
  const std::wstring expect_locator = L"C:/基地/assets/locator.png";
  if (CanEncodeInAcp(expect_locator)) {
    EXPECT_EQ(expect_locator, AnsiToWide(setting.locator_filename_ansi));
    EXPECT_EQ(L"C:/基地/assets/locator2.png",
              AnsiToWide(setting.locator2_filename_ansi));
  } else {
    ASSERT_FALSE(setting.locator_filename_ansi.empty());
    ASSERT_FALSE(setting.locator2_filename_ansi.empty());
  }
}
