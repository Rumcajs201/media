#pragma once
#include <stdint.h>

// Host-testable application traffic budget and graceful ITS close scheduler.
// No BLE or Arduino dependency; all time values use uint32_t millis() ticks.
class ItsSafeGuard {
public:
  void reset() {
    active_ = false; paused_ = false; closeIssued_ = false;
    sent_ = 0; lastRequest_ = 0; closeAt_ = 0;
  }
  void activate() { reset(); active_ = true; }
  bool allowRequest(uint32_t now, uint32_t minimumGapMs, uint16_t maximumCount) {
    if (!active_ || paused_ || sent_ >= maximumCount) return false;
    if (sent_ != 0 && (uint32_t)(now - lastRequest_) < minimumGapMs) return false;
    lastRequest_ = now;
    ++sent_;
    return true;
  }
  void pauseAndScheduleClose(uint32_t now, uint32_t graceMs) {
    if (!active_ || paused_) return;
    paused_ = true;
    closeAt_ = now + graceMs;
  }
  bool closeDue(uint32_t now) {
    if (!active_ || !paused_ || closeIssued_ ||
        (int32_t)(now - closeAt_) < 0) return false;
    closeIssued_ = true;
    active_ = false;
    return true;
  }
  bool paused() const { return paused_; }
  bool active() const { return active_; }
  uint16_t sentCount() const { return sent_; }
private:
  bool active_ = false;
  bool paused_ = false;
  bool closeIssued_ = false;
  uint16_t sent_ = 0;
  uint32_t lastRequest_ = 0;
  uint32_t closeAt_ = 0;
};
