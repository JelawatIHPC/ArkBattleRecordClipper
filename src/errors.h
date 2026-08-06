#pragma once

#include <mutex>
#include <string>
#include <vector>

/* 线程安全错误队列: 任意工作线程 Post 投递错误, 主线程 (UI) 轮询 Drain 取走。
*/
class ACErrorQueue {
public:
    ACErrorQueue() = default;
    ACErrorQueue(const ACErrorQueue& other) = delete;

    /* 向队列投递一条错误消息 (线程安全)
     *
     * @param message 错误消息文本
     */
    void Post(std::string message);

    /* 取出并清空全部未消费的错误消息 (线程安全)
     *
     * @return std::vector<std::string> 错误消息列表, 无错误时为空
     */
    std::vector<std::string> Drain();

private:
    std::mutex mutex_;
    std::vector<std::string> messages_;
};

/* 获取全局错误队列单例
 *
 * @return ACErrorQueue& 全局错误队列
 */
ACErrorQueue& ACErrors();
