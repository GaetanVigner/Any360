// SpinPacer: what a thread's zero delay (Sleep(0)) does, yield or a real sleep, so that a title's frame limiter does
// not spin a processor while it waits for its time.
//
// A title that waits for its frame time loops on `while (QueryPerformanceCounter() < deadline) Sleep(0);`. On the
// console Sleep(0) gives the hardware thread to its sibling; on the host it yields, and the loop spins a processor for
// all of the wait (in Mirror's Edge at its 62 FPS cap, a few milliseconds a frame). Sleep(0) says nothing of the
// deadline, but a frame limiter's waits end at a steady cadence. The pacer watches one thread's zero delays:
//
// - A streak is a run of zero delays with less than kStreakGap between the return of one and the next call (the
//   thread does nothing else meanwhile). It ends at its last call's return, seen at the next call after a gap.
// - When the last kPeriods streak ends are a steady cadence (each period within kJitter of their median, the median
//   between kMinPeriod and kMaxPeriod), the next streak is predicted to end one period after the last.
// - A zero delay earlier than the predicted end minus a margin sleeps until that point (one sleep a call: the title
//   calls again after its clock check); in the last margin it yields, so the title still sees its deadline itself.
//   The margin covers the host's sleep overshoot: kMinMargin plus the largest overshoot of the recent sleeps.
// - Safety: if a streak ends at the return of a sleep (no yield after it), the deadline may have passed during the
//   sleep: the margin doubles (up to kMaxMargin; halved again after kCleanEnds safe ends) and the cadence is learnt
//   again before the next sleep. A title that changes cadence to a shorter one (30 to 60 FPS) can have its first
//   wait of the new cadence held past its deadline once (by up to the old period), then the pacer stops. A streak that
//   ends before its prediction, or a period off the cadence, also clears it. Threads that yield without a cadence (a
//   job system, a lock's spin) only ever yield.
//
// The class takes the time from its caller (tests drive it with a simulated clock); Scheduler::Delay gives it the
// steady clock and sleeps what it says.
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>

namespace any360::sys {

// A sleep of `d` as precise as the host allows (Windows: a high-resolution waitable timer a thread; elsewhere
// nanosleep). False: no precise sleep on this host (the caller yields instead). precise_sleep.cpp.
bool PreciseSleep(std::chrono::nanoseconds d);

class SpinPacer {
 public:
  using Clock = std::chrono::steady_clock;
  using Duration = std::chrono::nanoseconds;
  static constexpr Duration kStreakGap = std::chrono::microseconds(200);
  static constexpr Duration kJitter = std::chrono::microseconds(250);
  static constexpr Duration kMinPeriod = std::chrono::milliseconds(2), kMaxPeriod = std::chrono::milliseconds(100);
  static constexpr Duration kMinMargin = std::chrono::microseconds(500), kMaxMargin = std::chrono::milliseconds(4);
  static constexpr Duration kMinSleep = std::chrono::microseconds(200);  // shorter: yield instead
  static constexpr int kPeriods = 8;
  static constexpr uint32_t kCleanEnds = 64;

  struct Stats {
    uint64_t calls = 0, sleeps = 0, slept_ns = 0, unsafe_ends = 0, streaks = 0;
  };

  // A zero delay starts at `now`: how long to sleep (zero: yield).
  Duration Delay(Clock::time_point now) {
    ++stats_.calls;
    if (has_return_ && now - last_return_ > kStreakGap) EndStreak();
    in_streak_ = true;
    if (!predicted_) return Duration::zero();
    const Duration until = (next_end_ - margin_) - now;
    if (until < kMinSleep) return Duration::zero();
    sleep_asked_ = until;
    sleep_started_ = now;
    return until;
  }

  // The zero delay that started with Delay returns at `now` (after the sleep or the yield).
  void Returned(Clock::time_point now) {
    if (sleep_asked_ > Duration::zero()) {
      ++stats_.sleeps;
      const Duration slept = now - sleep_started_;
      stats_.slept_ns += uint64_t(std::max<int64_t>(0, slept.count()));
      const Duration over = slept - sleep_asked_;
      overshoots_[overshoot_next_++ % overshoots_.size()] = std::max(Duration::zero(), over);
      Duration worst = Duration::zero();
      for (Duration o : overshoots_) worst = std::max(worst, o);
      margin_ = std::clamp(std::max(margin_floor_, kMinMargin + worst), kMinMargin, kMaxMargin);
      last_was_sleep_ = true;
    } else {
      last_was_sleep_ = false;
    }
    sleep_asked_ = Duration::zero();
    last_return_ = now;
    has_return_ = true;
  }

  Duration margin() const { return margin_; }
  bool predicting() const { return predicted_; }
  Duration period() const { return period_; }
  const Stats& stats() const { return stats_; }

 private:
  void EndStreak() {
    if (!in_streak_) return;
    in_streak_ = false;
    ++stats_.streaks;
    const Clock::time_point end = last_return_;
    if (last_was_sleep_) {
      // Ended at a sleep's return: the deadline may have passed while it slept. More margin, the cadence again.
      ++stats_.unsafe_ends;
      clean_ends_ = 0;
      margin_floor_ = std::min(kMaxMargin, std::max(margin_, margin_floor_) * 2);
      margin_ = std::max(margin_, margin_floor_);
      count_ = 0;
      predicted_ = false;
      has_end_ = true;
      last_end_ = end;
      return;
    }
    // A safe end (its last zero delay yielded): after kCleanEnds of them in a row the extra margin halves again (a
    // busy moment of the host passes).
    if (++clean_ends_ >= kCleanEnds) {
      clean_ends_ = 0;
      margin_floor_ = std::max(kMinMargin, margin_floor_ / 2);
    }
    if (has_end_) {
      const Duration p = end - last_end_;
      periods_[count_ % kPeriods] = p;
      ++count_;
    }
    has_end_ = true;
    last_end_ = end;
    predicted_ = false;
    if (count_ < kPeriods) return;
    std::array<Duration, kPeriods> sorted = periods_;
    std::nth_element(sorted.begin(), sorted.begin() + kPeriods / 2, sorted.end());
    const Duration median = sorted[kPeriods / 2];
    if (median < kMinPeriod || median > kMaxPeriod) return;
    for (Duration p : periods_) {
      if (p < median - kJitter || p > median + kJitter) return;
    }
    period_ = median;
    next_end_ = end + median;
    predicted_ = true;
  }

  Stats stats_{};
  bool has_return_ = false, in_streak_ = false, last_was_sleep_ = false, has_end_ = false, predicted_ = false;
  Clock::time_point last_return_{}, last_end_{}, next_end_{}, sleep_started_{};
  Duration sleep_asked_{0}, period_{0}, margin_{kMinMargin}, margin_floor_{kMinMargin};
  std::array<Duration, kPeriods> periods_{};
  std::array<Duration, 16> overshoots_{};
  uint32_t count_ = 0, overshoot_next_ = 0, clean_ends_ = 0;
};

}  // namespace any360::sys
