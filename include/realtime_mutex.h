#pragma once
#include <pthread.h>
#include <mutex>
#include <system_error>

namespace robobaton_demo {
// Bounded queue owners may run below their realtime producers. Inheritance
// lets a preempted owner finish the critical section before the producer resumes.
class RealtimeMutex {
 public:
  RealtimeMutex() {
    pthread_mutexattr_t attributes;
    int result = pthread_mutexattr_init(&attributes);
    if (result) throw std::system_error(result, std::generic_category(), "mutex attributes");
    result = pthread_mutexattr_setprotocol(&attributes, PTHREAD_PRIO_INHERIT);
    if (!result) result = pthread_mutex_init(&mutex_, &attributes);
    pthread_mutexattr_destroy(&attributes);
    if (result) throw std::system_error(result, std::generic_category(), "priority inheritance mutex");
  }
  ~RealtimeMutex() { pthread_mutex_destroy(&mutex_); }
  RealtimeMutex(const RealtimeMutex&) = delete;
  RealtimeMutex& operator=(const RealtimeMutex&) = delete;
  void lock() {
    const int result = pthread_mutex_lock(&mutex_);
    if (result) throw std::system_error(result, std::generic_category(), "mutex lock");
  }
  void unlock() noexcept { pthread_mutex_unlock(&mutex_); }
  pthread_mutex_t* native_handle() noexcept { return &mutex_; }
 private:
  pthread_mutex_t mutex_;
};

// pthread_cond_wait atomically releases the same PI mutex used by notifiers;
// no hidden normal-priority lock is introduced on the wakeup path.
class RealtimeCondition {
 public:
  RealtimeCondition() = default;
  ~RealtimeCondition() { pthread_cond_destroy(&condition_); }
  RealtimeCondition(const RealtimeCondition&) = delete;
  RealtimeCondition& operator=(const RealtimeCondition&) = delete;
  void notify_one() noexcept { pthread_cond_signal(&condition_); }
  void notify_all() noexcept { pthread_cond_broadcast(&condition_); }
  template<class Predicate>
  void wait(std::unique_lock<RealtimeMutex>& lock, Predicate ready) {
    while (!ready()) {
      const int result = pthread_cond_wait(&condition_, lock.mutex()->native_handle());
      if (result) throw std::system_error(result, std::generic_category(), "condition wait");
    }
  }
 private:
  pthread_cond_t condition_ = PTHREAD_COND_INITIALIZER;
};
}
