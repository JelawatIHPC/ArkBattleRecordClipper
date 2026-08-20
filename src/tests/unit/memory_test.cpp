#include <gtest/gtest.h>

#include "memory.h"

/* 数据包池: 分配返回非空指针
 */
TEST(ACPacketPool, AllocateReturnsNonNull) {
  ACPacketPool pool;
  AVPacket* packet = pool.Allocate();
  ASSERT_NE(nullptr, packet);
  pool.Free(packet);
}

/* 数据包池: Free 后再次 Allocate 复用同一指针
 */
TEST(ACPacketPool, FreeThenAllocateReusesPointer) {
  ACPacketPool pool;
  AVPacket* first = pool.Allocate();
  pool.Free(first);

  AVPacket* second = pool.Allocate();
  EXPECT_EQ(first, second);
  pool.Free(second);
}

/* 数据包池: Free 非本池分配的指针为无操作
 */
TEST(ACPacketPool, FreeOnForeignPointerIsNoOp) {
  ACPacketPool pool;
  AVPacket* foreign = av_packet_alloc();
  EXPECT_NO_THROW(pool.Free(foreign));
  av_packet_free(&foreign);

  AVPacket* packet = pool.Allocate();
  EXPECT_NE(nullptr, packet);
  pool.Free(packet);
}

/* 帧池: 分配返回非空指针
 */
TEST(ACFramePool, AllocateReturnsNonNull) {
  ACFramePool pool;
  AVFrame* frame = pool.Allocate();
  ASSERT_NE(nullptr, frame);
  pool.Free(frame);
}

/* 帧池: Free 后再次 Allocate 复用同一指针
 */
TEST(ACFramePool, FreeThenAllocateReusesPointer) {
  ACFramePool pool;
  AVFrame* first = pool.Allocate();
  pool.Free(first);

  AVFrame* second = pool.Allocate();
  EXPECT_EQ(first, second);
  pool.Free(second);
}
