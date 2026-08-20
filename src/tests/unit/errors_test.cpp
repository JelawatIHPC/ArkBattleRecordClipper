#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "errors.h"

/* 错误队列: 投递与取走按顺序
 */
TEST(ACErrorQueue, PostAndDrainInOrder) {
  ACErrorQueue queue;
  queue.Post("err1");
  queue.Post("err2");

  std::vector<std::string> messages = queue.Drain();
  ASSERT_EQ(2u, messages.size());
  EXPECT_EQ("err1", messages[0]);
  EXPECT_EQ("err2", messages[1]);
}

/* 错误队列: Drain 会清空队列
 */
TEST(ACErrorQueue, DrainClearsQueue) {
  ACErrorQueue queue;
  queue.Post("err1");
  queue.Drain();

  EXPECT_TRUE(queue.Drain().empty());
}

/* 错误队列: 空队列 Drain 返回空列表
 */
TEST(ACErrorQueue, DrainOnEmptyQueueReturnsEmpty) {
  ACErrorQueue queue;
  EXPECT_TRUE(queue.Drain().empty());
}

/* 错误队列: 多线程并发投递全部可被收集
 */
TEST(ACErrorQueue, ConcurrentPostsAreAllCollected) {
  ACErrorQueue queue;
  constexpr int kThreadCount = 8;
  constexpr int kMessagesPerThread = 100;

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreadCount; ++t) {
    threads.emplace_back([&queue]() {
      for (int i = 0; i < kMessagesPerThread; ++i) {
        queue.Post("msg");
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  EXPECT_EQ(kThreadCount * kMessagesPerThread,
            static_cast<int>(queue.Drain().size()));
}
