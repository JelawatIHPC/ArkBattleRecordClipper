#pragma once

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
