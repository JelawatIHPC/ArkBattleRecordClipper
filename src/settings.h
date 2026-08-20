#pragma once

#include <string>

#include "core.h"

/* 解析前端传入的 JSON 参数为 Setting (纯函数, 便于单元测试)
 *
 * @param req JSON 字符串, 形如 {"input_file": "...", "output_file": "...", ...},
 *            实际接收时外层套有一层数组, 由本函数自动取出
 * @param base_dir 基准目录 (UTF-16 宽字符): 输出文件与定位模板均相对此目录解析,
 *                 调用方必须传入 Wide 路径 (如 GetModuleFileNameW / Utf8ToWide 的结果)
 * @return Setting 解析后的配置: 输入/输出文件名为 UTF-8 (供 FFmpeg),
 *                 定位模板为 ANSI 系统代码页 (供 OpenCV)
 * @throw std::runtime_error 参数解析失败时抛出
 */
Setting ParseSetting(const std::string& req, const std::wstring& base_dir);

/* 解析前端传入的 JSON 参数为 Setting, 基准目录取程序所在目录 (Windows)
 *
 * @param req JSON 字符串
 * @return Setting 解析后的配置: 输入/输出文件名为 UTF-8 (供 FFmpeg),
 *                 定位模板为 ANSI 系统代码页 (供 OpenCV)
 * @throw std::runtime_error 参数解析失败时抛出
 */
Setting ParseSetting(const std::string& req);
