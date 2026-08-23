/**
 * @file main.cpp
 * @brief 应用程序入口文件
 *
 * 使用 webview 库创建无边框窗口并加载本地 HTML UI。
 */

#include "core.h"

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

#include <webview/webview.h>
#include <nlohmann/json.hpp>

#include "errors.h"
#include "crashguard.h"
#include "settings.h"

// 窗口尺寸常量
constexpr int WINDOW_WIDTH = 864;
constexpr int WINDOW_HEIGHT = 648;


// 通过 XMake 嵌入的二进制对象
extern "C" {
  extern const char _binary_index_html_start[];
  extern const char _binary_index_html_end[];
}
static const std::string ui_html(_binary_index_html_start, _binary_index_html_end);


// 全局变量用于拖动
static bool isDragging = false;
static POINT dragStartCursorPos;
static POINT dragStartWindowPos;
static HWND g_hwnd = nullptr;

// 处理状态标志: 启动前置位, 处理线程结束或抛异常后复位
// 注意: 不能用 std::mutex 实现该语义——UI 线程加锁 + 工作线程解锁是跨线程解锁,
// 属未定义行为, debug CRT 会在 _Mtx_unlock 中以 "unlock of unowned mutex" 断言终止进程
static std::atomic<bool> g_processing{ false };

// MSVC 隐藏控制台窗口
#ifdef _MSVC_STL_VERSION
#pragma comment(linker, "/subsystem:windows /entry:mainCRTStartup")
#endif

/**
 * @brief 应用程序入口函数
 *
 * 创建无边框窗口，并加载 ui/index.html。
 *
 * @return int 程序退出码，0 表示正常退出
 */
int main() {
  // SEH Mechanism: crash.log & minidump
  InstallCrashGuard();

  // 创建 webview 实例
  webview::webview w(true, nullptr);
  
  // 设置窗口标题
  w.set_title("ArkBattleRecordClipper");

  // 设置窗口大小
  w.set_size(WINDOW_WIDTH, WINDOW_HEIGHT, WEBVIEW_HINT_NONE);

  // 获取原生窗口句柄并设置为无边框样式
  HWND hwnd = static_cast<HWND>(w.window().value());
  g_hwnd = hwnd;
  
  if (hwnd != nullptr) {
    // 移除标题栏和边框
    LONG style = GetWindowLong(hwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MAXIMIZEBOX |
               WS_MINIMIZEBOX);
    SetWindowLong(hwnd, GWL_STYLE, style);

    // 设置扩展样式，使窗口支持透明和层叠效果
    LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
    exStyle |= WS_EX_LAYERED;
    SetWindowLong(hwnd, GWL_EXSTYLE, exStyle);

    // 使用 DwmSetWindowAttribute 设置窗口圆角（Windows 11 风格）
    const DWM_WINDOW_CORNER_PREFERENCE cornerPref = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));

    // 重新应用样式并刷新窗口
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE);
  }

  // 绑定 JavaScript 函数：开始拖动
  w.bind("startDrag", [&w](const std::string& req) -> std::string {
    isDragging = true;
    
    // 获取当前鼠标位置（屏幕坐标）
    GetCursorPos(&dragStartCursorPos);
    
    // 获取窗口当前位置
    RECT rect;
    HWND hwnd = static_cast<HWND>(w.window().value());
    GetWindowRect(hwnd, &rect);
    dragStartWindowPos.x = rect.left;
    dragStartWindowPos.y = rect.top;
    
    return "";
  });

  // 绑定 JavaScript 函数：执行拖动
  w.bind("doDrag", [&w](const std::string& req) -> std::string {
    if (!isDragging) return "";
    
    POINT currentCursorPos;
    GetCursorPos(&currentCursorPos);
    
    // 计算偏移量
    int offsetX = currentCursorPos.x - dragStartCursorPos.x;
    int offsetY = currentCursorPos.y - dragStartCursorPos.y;
    
    // 移动窗口
    HWND hwnd = static_cast<HWND>(w.window().value());
    SetWindowPos(hwnd, nullptr,
                 dragStartWindowPos.x + offsetX,
                 dragStartWindowPos.y + offsetY,
                 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    
    return "";
  });

  // 绑定 JavaScript 函数：结束拖动
  w.bind("endDrag", [](const std::string& req) -> std::string {
    isDragging = false;
    return "";
  });

  // 绑定 JavaScript 函数：最小化窗口
  w.bind("minimizeWindow", [&w](const std::string& req) -> std::string {
    HWND hwnd = static_cast<HWND>(w.window().value());
    ShowWindow(hwnd, SW_MINIMIZE);
    return "";
  });

  // 绑定 JavaScript 函数：关闭窗口
  w.bind("closeWindow", [&w](const std::string& req) -> std::string {
    HWND hwnd = static_cast<HWND>(w.window().value());
    PostMessage(hwnd, WM_CLOSE, 0, 0);
    return "";
  });

  // 绑定 JavaScript 函数：选择文件
  w.bind("selectVideoFile", [&w](const std::string& req) -> std::string {
    HWND hwnd = static_cast<HWND>(w.window().value());

    // 设置文件过滤器（UTF-8 编码）
    const wchar_t* filter = L"视频文件\0*.mp4;*.avi;*.mkv;*.mov;*.wmv;*.flv;*.webm;*.m4v;*.mpeg;*.mpg\0所有文件\0*.*\0";

    // 使用 4096 字节缓冲区支持长路径
    wchar_t filePath[4096] = {0};

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = 4096;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = L"选择视频文件";

    // 显示文件选择对话框（使用 Unicode 版本）
    if (GetOpenFileNameW(&ofn)) {
      // 将宽字符路径转换为 UTF-8
      int utf8Len = WideCharToMultiByte(CP_UTF8, 0, filePath, -1, nullptr, 0, nullptr, nullptr);
      if (utf8Len > 0) {
        std::vector<char> utf8Path(utf8Len);
        WideCharToMultiByte(CP_UTF8, 0, filePath, -1, utf8Path.data(), utf8Len, nullptr, nullptr);
        std::string result(utf8Path.data());
        // 转义反斜杠为双反斜杠
        size_t pos = 0;
        while ((pos = result.find('\\', pos)) != std::string::npos) {
          result.insert(pos, "\\");
          pos += 2;
        }
        // 转义双引号
        pos = 0;
        while ((pos = result.find('"', pos)) != std::string::npos) {
          result.insert(pos, "\\");
          pos += 2;
        }
        return "\"" + result + "\"";
      }
    }

    // 用户取消选择，返回 null
    return "null";
  });

  // 绑定 JavaScript 函数：启动处理
  w.bind("startProcessing", [](const std::string& req) -> std::string {
    // 解析 JSON 请求
    Setting setting;
    try {
      setting = ParseSetting(req);
    } catch (const std::exception& e) {
      // 参数解析失败, 投递到错误队列
      ACErrors().Post(e.what());
      return "false";
    }

    // 已有处理进行中则拒绝本次启动
    if (g_processing.exchange(true)) {
      ACErrors().Post("重复启动");
      return "false";
    }

    // 在新线程中启动处理
    try {
      std::thread processingThread([setting]() {
        try {
          Start(setting);   // 调用 core 模块的 Start 函数
        } catch (const std::exception& e) {
          ACErrors().Post(e.what());
        } catch (...) {
          ACErrors().Post("未知错误");
        }
        g_processing.store(false);
      });
      processingThread.detach();
    } catch (const std::system_error&) {
      // 线程创建失败, 复位标志避免永久"重复启动"
      g_processing.store(false);
      ACErrors().Post("启动处理时发生未知错误");
      return "false";
    }
    
    return "true";
  });

  // 绑定 JavaScript 函数：获取进度指标
  w.bind("getProgressMetrics", [](const std::string& req) -> std::string {
    nlohmann::json result;
    
    // 直接调用 core 模块的 GetProgressMetrics 函数
    ProgressMetrics metrics = GetProgressMetrics();
    
    // 返回 JSON 格式的进度指标
    result["state"] = metrics.state;
    result["progress"] = metrics.progress_percent;
    result["fps"] = metrics.frames_per_second;
    result["qd"] = metrics.queue_depth;
    result["eta"] = metrics.eta_seconds;
    // 附带并取走全部待消费的错误消息
    result["errors"] = ACErrors().Drain();
    return result.dump();
  });

  // 绑定 JavaScript 函数：输入文件改变通知
  w.bind("onInputfileChanged", [](const std::string& req) -> std::string {
    try {
      // 解析 JSON 请求并通知预分析器
      OnInputfileChanged(ParseSetting(req));
    } catch (const std::exception& e) {
      // 解析或启动预分析失败, 投递到错误队列
      ACErrors().Post(e.what());
    }

    return "true";
  });

  // 运行应用程序
  w.set_html(ui_html);
  w.run();

  return 0;
}
