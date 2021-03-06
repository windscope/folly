/*
 * Copyright 2016 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/* -*- Mode: C++; tab-width: 2; c-basic-offset: 2; indent-tabs-mode: nil -*- */

#include <atomic>
#include <thread>
#include <mutex>
#include <folly/Memory.h>
#include <condition_variable>
#include <gtest/gtest.h>

#include <folly/Baton.h>
#include <folly/experimental/RCURefCount.h>
#include <folly/experimental/ReadMostlySharedPtr.h>

using folly::ReadMostlyMainPtr;
using folly::ReadMostlyWeakPtr;
using folly::ReadMostlySharedPtr;

// send SIGALRM to test process after this many seconds
const unsigned int TEST_TIMEOUT = 10;

class ReadMostlySharedPtrTest : public ::testing::Test {
 public:
  ReadMostlySharedPtrTest() {
    alarm(TEST_TIMEOUT);
  }
};

struct TestObject {
  int value;
  std::atomic<int>& counter;

  TestObject(int value, std::atomic<int>& counter)
      : value(value), counter(counter) {
    ++counter;
  }

  ~TestObject() {
    assert(counter.load() > 0);
    --counter;
  }
};

// One side calls requestAndWait(), the other side calls waitForRequest(),
// does something and calls completed().
class Coordinator {
 public:
  void requestAndWait() {
    requestBaton_.post();
    completeBaton_.wait();
  }

  void waitForRequest() {
    folly::RCURegisterThread();
    requestBaton_.wait();
  }

  void completed() {
    completeBaton_.post();
  }

 private:
  folly::Baton<> requestBaton_;
  folly::Baton<> completeBaton_;
};

TEST_F(ReadMostlySharedPtrTest, BasicStores) {
  ReadMostlyMainPtr<TestObject> ptr;

  // Store 1.
  std::atomic<int> cnt1{0};
  ptr.reset(folly::make_unique<TestObject>(1, cnt1));
  EXPECT_EQ(1, cnt1.load());

  // Store 2, check that 1 is destroyed.
  std::atomic<int> cnt2{0};
  ptr.reset(folly::make_unique<TestObject>(2, cnt2));
  EXPECT_EQ(1, cnt2.load());
  EXPECT_EQ(0, cnt1.load());

  // Store nullptr, check that 2 is destroyed.
  ptr.reset(nullptr);
  EXPECT_EQ(0, cnt2.load());
}

TEST_F(ReadMostlySharedPtrTest, BasicLoads) {
  std::atomic<int> cnt2{0};
  ReadMostlySharedPtr<TestObject> x;

  {
    ReadMostlyMainPtr<TestObject> ptr;

    // Check that ptr is initially nullptr.
    EXPECT_EQ(ptr.get(), nullptr);

    std::atomic<int> cnt1{0};
    ptr.reset(folly::make_unique<TestObject>(1, cnt1));
    EXPECT_EQ(1, cnt1.load());

    x = ptr;
    EXPECT_EQ(1, x->value);

    ptr.reset(folly::make_unique<TestObject>(2, cnt2));
    EXPECT_EQ(1, cnt2.load());
    EXPECT_EQ(1, cnt1.load());

    x = ptr;
    EXPECT_EQ(2, x->value);
    EXPECT_EQ(0, cnt1.load());

    ptr.reset(nullptr);
    EXPECT_EQ(1, cnt2.load());
  }

  EXPECT_EQ(1, cnt2.load());

  x.reset();
  EXPECT_EQ(0, cnt2.load());
}

TEST_F(ReadMostlySharedPtrTest, LoadsFromThreads) {
  std::atomic<int> cnt{0};

  {
    ReadMostlyMainPtr<TestObject> ptr;
    Coordinator loads[7];

    std::thread t1([&] {
      loads[0].waitForRequest();
      EXPECT_EQ(ptr.getShared(), nullptr);
      loads[0].completed();

      loads[3].waitForRequest();
      EXPECT_EQ(2, ptr.getShared()->value);
      loads[3].completed();

      loads[4].waitForRequest();
      EXPECT_EQ(4, ptr.getShared()->value);
      loads[4].completed();

      loads[5].waitForRequest();
      EXPECT_EQ(5, ptr.getShared()->value);
      loads[5].completed();
    });

    std::thread t2([&] {
      loads[1].waitForRequest();
      EXPECT_EQ(1, ptr.getShared()->value);
      loads[1].completed();

      loads[2].waitForRequest();
      EXPECT_EQ(2, ptr.getShared()->value);
      loads[2].completed();

      loads[6].waitForRequest();
      EXPECT_EQ(5, ptr.getShared()->value);
      loads[6].completed();
    });

    loads[0].requestAndWait();

    ptr.reset(folly::make_unique<TestObject>(1, cnt));
    loads[1].requestAndWait();

    ptr.reset(folly::make_unique<TestObject>(2, cnt));
    loads[2].requestAndWait();
    loads[3].requestAndWait();

    ptr.reset(folly::make_unique<TestObject>(3, cnt));
    ptr.reset(folly::make_unique<TestObject>(4, cnt));
    loads[4].requestAndWait();

    ptr.reset(folly::make_unique<TestObject>(5, cnt));
    loads[5].requestAndWait();
    loads[6].requestAndWait();

    EXPECT_EQ(1, cnt.load());

    t1.join();
    t2.join();
  }

  EXPECT_EQ(0, cnt.load());
}

TEST_F(ReadMostlySharedPtrTest, Ctor) {
  std::atomic<int> cnt1{0};
  {
    ReadMostlyMainPtr<TestObject> ptr(
      folly::make_unique<TestObject>(1, cnt1));

    EXPECT_EQ(1, ptr.getShared()->value);
  }

  EXPECT_EQ(0, cnt1.load());
}

TEST_F(ReadMostlySharedPtrTest, ClearingCache) {
  ReadMostlyMainPtr<TestObject> ptr;

  // Store 1.
  std::atomic<int> cnt1{0};
  ptr.reset(folly::make_unique<TestObject>(1, cnt1));

  Coordinator c;

  std::thread t([&] {
    // Cache the pointer for this thread.
    ptr.getShared();
    c.requestAndWait();
  });

  // Wait for the thread to cache pointer.
  c.waitForRequest();
  EXPECT_EQ(1, cnt1.load());

  // Store 2 and check that 1 is destroyed.
  std::atomic<int> cnt2{0};
  ptr.reset(folly::make_unique<TestObject>(2, cnt2));
  EXPECT_EQ(0, cnt1.load());

  // Unblock thread.
  c.completed();
  t.join();
}
