#include "settings.h"

#include <stdexcept>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace {

/* UTF-8 字符串转 UTF-16 宽字符
 *
 * @param utf8 UTF-8 编码字符串
 * @return UTF-16 宽字符字符串, 转换失败时为空串
 */
std::wstring Utf8ToWide(const std::string& utf8) {
  if (utf8.empty()) {
    return {};
  }
  int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
  if (len <= 0) {
    return {};
  }
  std::wstring wide((size_t)len, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), wide.data(), len);
  return wide;
}

/* UTF-16 宽字符转 UTF-8 (FFmpeg 路径要求严格 UTF-8)
 *
 * @param wide UTF-16 宽字符字符串
 * @return UTF-8 编码字符串, 转换失败时为空串
 */
std::string WideToUtf8(const std::wstring& wide) {
  if (wide.empty()) {
    return {};
  }
  int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
  if (len <= 0) {
    return {};
  }
  std::string utf8((size_t)len, '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), utf8.data(), len, nullptr, nullptr);
  return utf8;
}

/* UTF-16 宽字符转 ANSI 系统代码页 (OpenCV imread 内部按 ANSI fopen, 中文系统即 GBK)
 *
 * @param wide UTF-16 宽字符字符串
 * @return ANSI 编码字符串, 转换失败时为空串
 */
std::string WideToAnsi(const std::wstring& wide) {
  if (wide.empty()) {
    return {};
  }
  int len = WideCharToMultiByte(CP_ACP, 0, wide.c_str(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
  if (len <= 0) {
    return {};
  }
  std::string ansi((size_t)len, '\0');
  WideCharToMultiByte(CP_ACP, 0, wide.c_str(), (int)wide.size(), ansi.data(), len, nullptr, nullptr);
  return ansi;
}

}  // namespace

Setting ParseSetting(const std::string& req, const std::wstring& base_dir) {
  // 解析 JSON 请求
  // req 格式: {"input_file": "...", "output_file": "...", "bitrate": 6000, ...}

  // 创建 Setting 结构体
  Setting setting;

  setting.locator_filename_ansi = WideToAnsi(base_dir + L"/assets/locator.png");
  setting.locator2_filename_ansi = WideToAnsi(base_dir + L"/assets/locator2.png");

  try {
    // 我也不知道为什么 webview 弄过来的 json 还套了一层数组
    nlohmann::json j = nlohmann::json::parse(req)[0];

    // 提取参数, JSON 路径先统一转为 Wide, 再按消费方字符集分发
    setting.input_filename_utf8 = WideToUtf8(Utf8ToWide(j.value("input_file", "")));
    setting.output_filename_utf8 = WideToUtf8(base_dir + L"/" + Utf8ToWide(j.value("output_file", "")));
    setting.output_bitrate = j.value("bitrate", 0);
    setting.acceleration.play1x = j.value("speed_1x", false) ? 2.0f : 1.0f;
    setting.acceleration.select = j.value("bullet_time", 1.0f);
    setting.select_pause_reserved_time = j.value("anim_reserved", 0.3f);
    setting.encoder = j.value("encoder", "auto");
    setting.decoder = j.value("decoder", "dxva2");
  } catch (const std::exception& e) {
    // 解析失败直接抛出, 由调用方投递到错误队列
    throw std::runtime_error(std::string("参数解析错误: ") + e.what());
  }
  return setting;
}

Setting ParseSetting(const std::string& req) {
  // 获取当前工作目录, 直接使用 Wide API 得到 UTF-16 宽字符路径
  wchar_t current_path[4096];
  GetModuleFileNameW(NULL, current_path, 4096);
  std::wstring current_dir(current_path);
  current_dir = current_dir.substr(0, current_dir.find_last_of(L"\\"));

  return ParseSetting(req, current_dir);
}