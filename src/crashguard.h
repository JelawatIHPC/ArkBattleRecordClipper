#pragma once

#include <cstddef>

/**
 * @brief 安装崩溃捕获 (SEH 顶层过滤器)
 *
 * 捕获进程内任意线程未处理的结构化异常 (访问违例、除零、非法指令、栈溢出等),
 * 在进程死亡前:
 *   1. 在 exe 所在目录写入 crash.log (时间 / PID / TID / 异常码 / 异常地址 / 所属模块 / 栈地址);
 *   2. 通过 dbghelp.dll (运行时延迟加载) 写入 minidump 文件, 文件名 crash-YYYYMMDD-HHMMSS.dmp;
 *   3. 以退出码 0xAC000001 结束进程, 避免携带已损坏的状态继续运行。
 *
 * 注意: crash.log / minidump 仅在崩溃发生时创建, 正常运行不会产生任何日志文件。
 */
void InstallCrashGuard();

/**
 * @brief 崩溃上下文回调函数类型
 *
 * 崩溃时由 crashguard 在崩溃线程内同步调用一次, 把业务现场信息写入 buf。
 * 实现约束: 禁止抛异常、禁止加锁、禁止动态分配, 只做原子读取与格式化。
 *
 * @param buf 输出缓冲区 (调用方保证至少 len 字节)
 * @param len 缓冲区长度
 */
using CrashContextFn = void (*)(char* buf, size_t len);

/**
 * @brief 注册崩溃上下文回调 (进程级, 由业务模块在初始化时调用一次)
 *
 * 回调在每次崩溃记录时被调用, 其输出以 "context=" 行写入 crash.log,
 * 用于判读崩溃时的业务现场 (处理阶段 / 帧下标 / 队列规模等)。
 * 未注册时 crash.log 不含 context 行。多次调用以最后一次为准。
 *
 * @param fn 回调函数指针, 传 nullptr 取消注册
 */
void SetCrashContextProvider(CrashContextFn fn);
