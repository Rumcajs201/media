#pragma once
#include <stdint.h>
// Hardware-independent ITS/BLE fault policy. No automatic retry or scan.
class ItsSafetyGuard {
 public:
  enum class Stage : uint8_t { Idle, Active, Grace, Close, WaitClose, WaitBle, Locked };
  static constexpr uint32_t kSessionLimit=180000UL;
  static constexpr uint32_t kGrace=1500UL;
  static constexpr uint32_t kCloseGap=300UL;
  static constexpr uint32_t kDisconnectTimeout=5000UL;
  static constexpr uint32_t kCooldown=60000UL;
  Stage phase() const {return phase_;}
  bool active() const {return phase_==Stage::Active;}
  bool awaitingBle() const {return phase_==Stage::WaitBle;}
  bool canStart(uint32_t now,bool bleConnected) const {
    return !bleConnected && (phase_==Stage::Idle ||
      (phase_==Stage::Locked && due(now,deadline_)));
  }
  void start(uint32_t now){phase_=Stage::Active;sessionAt_=now;}
  bool sessionExpired(uint32_t now) const {
    return active() && uint32_t(now-sessionAt_)>=kSessionLimit;
  }
  void stop(uint32_t now,bool allowGrace){
    if(!active())return;
    phase_=allowGrace?Stage::Grace:Stage::Close;
    deadline_=now+(allowGrace?kGrace:0UL);
  }
  bool shouldClose(uint32_t now){
    if(phase_==Stage::Grace && due(now,deadline_))phase_=Stage::Close;
    return phase_==Stage::Close;
  }
  void closed(uint32_t now){
    if(phase_!=Stage::Close)return;
    phase_=Stage::WaitClose;deadline_=now+kCloseGap;
  }
  bool shouldDisconnect(uint32_t now) const {
    return phase_==Stage::WaitClose && due(now,deadline_);
  }
  void disconnectIssued(uint32_t now){
    if(phase_!=Stage::WaitClose)return;
    phase_=Stage::WaitBle;deadline_=now+kDisconnectTimeout;
  }
  bool timedOut(uint32_t now) const {
    return phase_==Stage::WaitBle && due(now,deadline_);
  }
  void linkDown(uint32_t now){
    if(phase_!=Stage::Idle && phase_!=Stage::Locked)lock(now);
  }
  void lock(uint32_t now){phase_=Stage::Locked;deadline_=now+kCooldown;}
  uint32_t remaining(uint32_t now) const {
    return phase_==Stage::Locked && !due(now,deadline_)?
      uint32_t(deadline_-now):0UL;
  }
 private:
  static bool due(uint32_t a,uint32_t b){return int32_t(a-b)>=0;}
  Stage phase_=Stage::Idle;
  uint32_t deadline_=0,sessionAt_=0;
};
