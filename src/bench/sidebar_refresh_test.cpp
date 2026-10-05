#include "../app/sidebar_refresh_schedule.h"
#include <cstdio>

int main() {
    using pulse::app::SidebarRefreshSchedule;
    bool passed = true;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        passed = passed && ok;
    };
    SidebarRefreshSchedule schedule;
    schedule.Request(true);
    check(schedule.Begin() == true, "initial request rebuilds sidebar");
    for (uint64_t now = 0; now < 120000; now += 25) schedule.Tick(now);
    check(!schedule.Begin(), "timer does not overlap or queue behind slow read");
    schedule.Complete(120000);
    check(!schedule.Begin(), "slow read does not trigger an immediate timer retry");
    schedule.Tick(149999);
    check(!schedule.Begin(), "capacity polling waits thirty seconds after completion");
    schedule.Tick(150000);
    check(schedule.Begin() == false, "periodic refresh queries capacity only");
    for (int i = 0; i < 100; ++i) schedule.Request(false);
    check(!schedule.Begin(), "operation and F5 bursts do not overlap current read");
    schedule.Complete(150100);
    check(schedule.Begin() == false, "event during read causes one follow-up capacity query");
    schedule.Complete(150200);
    check(!schedule.Begin(), "event burst coalesces to a single follow-up");
    schedule.Request(false);
    check(schedule.Begin() == false, "manual refresh starts immediately");
    schedule.Request(true);
    schedule.Request(false);
    check(schedule.RebuildPending(), "device or language rebuild supersedes stale snapshot");
    schedule.Complete(150300);
    check(schedule.Begin() == true, "capacity request cannot downgrade pending rebuild");
    schedule.Complete(150400);
    check(!schedule.RebuildPending() && !schedule.Begin(), "completed rebuild clears pending state");
    return passed ? 0 : 1;
}
