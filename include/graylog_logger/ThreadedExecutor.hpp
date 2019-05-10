/* Copyright (C) 2018 European Spallation Source, ERIC. See LICENSE file */
//===----------------------------------------------------------------------===//
///
/// \file
///
/// \brief Simple threaded executor to simplify the use of threading in this lib
/// library.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "graylog_logger/ConcurrentQueue.hpp"
#include <functional>
#include <thread>

namespace Log {
class ThreadedExecutor {
private:
public:
  using WorkMessage = std::function<void()>;
  ThreadedExecutor() : WorkerThread(ThreadFunction) {}
  ~ThreadedExecutor() {
    SendWork([=]() { RunThread = false; });
    WorkerThread.join();
  }
  void SendWork(WorkMessage Message) { MessageQueue.push(Message); }

private:
  bool RunThread{true};
  std::function<void()> ThreadFunction{[=]() {
    while (RunThread) {
      WorkMessage CurrentMessage;
      MessageQueue.wait_and_pop(CurrentMessage);
      CurrentMessage();
    }
  }};
  ConcurrentQueue<WorkMessage> MessageQueue;
  std::thread WorkerThread;
};

} // namespace Log