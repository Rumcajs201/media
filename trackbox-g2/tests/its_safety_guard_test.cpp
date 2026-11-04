#include "../TrackBox_Gen2/ItsSafetyGuard.h"
#include <cassert>
#include <iostream>
using G=ItsSafetyGuard;
int main() {
  unsigned n=0;
  auto ok=[&](bool b){assert(b);++n;};
  G g;uint32_t t=1000;
  ok(g.canStart(t,false));g.start(t);ok(g.active());ok(!g.canStart(t,false));
  g.stop(t,true);ok(g.phase()==G::Stage::Grace);
  ok(!g.shouldClose(t+1499));ok(g.shouldClose(t+1500));
  g.closed(t+1500);ok(!g.shouldClose(t+1500));
  ok(!g.shouldDisconnect(t+1799));ok(g.shouldDisconnect(t+1800));
  g.disconnectIssued(t+1800);ok(!g.timedOut(t+6799));
  ok(g.timedOut(t+6800));g.linkDown(t+2000);
  ok(g.phase()==G::Stage::Locked);
  ok(!g.canStart(t+61999,false));ok(g.canStart(t+62000,false));
  g.start(t+62000);ok(g.active());ok(!g.sessionExpired(t+62000+179999));
  ok(g.sessionExpired(t+62000+180000));
  g.stop(t+242000,false);ok(g.shouldClose(t+242000));
  g.closed(t+242000);ok(!g.shouldClose(t+242000));
  ok(g.phase()==G::Stage::WaitClose);
  ok(g.shouldDisconnect(t+242300));g.disconnectIssued(t+242300);
  ok(g.phase()==G::Stage::WaitBle);g.lock(t+248000);
  ok(!g.canStart(t+300000,true)); // BLE stuck: block retry regardless of cooldown
  ok(g.canStart(t+308000,false));
  G roll;uint32_t start=0xFFFFFF00UL;
  roll.start(start);roll.stop(start,true);
  ok(!roll.shouldClose(start+1499));ok(roll.shouldClose(start+1500));
  roll.closed(start+1500);ok(roll.shouldDisconnect(start+1800));
  roll.disconnectIssued(start+1800);roll.linkDown(start+2000);
  ok(!roll.canStart(start+61999,false));ok(roll.canStart(start+62000,false));
  G lost;lost.start(20);lost.linkDown(25);
  ok(lost.phase()==G::Stage::Locked);
  ok(!lost.shouldClose(300000));
  std::cout<<"PASS "<<n<<" assertions on real C++ state guard (no radio emulator)\n";
}
