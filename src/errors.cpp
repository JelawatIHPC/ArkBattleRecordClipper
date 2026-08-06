#include "errors.h"

#include <utility>

void ACErrorQueue::Post(std::string message) {
    std::lock_guard<std::mutex> lock(mutex_);
    messages_.push_back(std::move(message));
}

std::vector<std::string> ACErrorQueue::Drain() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> drained;
    messages_.swap(drained);
    return drained;
}

ACErrorQueue& ACErrors() {
    /* 实例位于 errors.cpp 中, 在堆上分配且永不释放, 保证程序退出时
     * 仍有运行的 detached 工作线程投递错误也不会访问已析构的对象。
     */
    static ACErrorQueue* queue = new ACErrorQueue;
    return *queue;
}
