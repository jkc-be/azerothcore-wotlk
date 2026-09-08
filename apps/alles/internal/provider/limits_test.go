package provider

import (
	"testing"
	"time"
)

func TestCallPolicyChangesRetainUsageAndDrainConcurrency(t *testing.T) {
	var limits CallLimits
	now := time.Unix(100, 0)
	if err := limits.Update(CallPolicy{"run", 1, "limited", 2, 2}); err != nil {
		t.Fatal(err)
	}
	if !limits.acquire(now) || !limits.acquire(now) {
		t.Fatal("two independent calls should fit")
	}
	if err := limits.Update(CallPolicy{"run", 2, "unlimited", 2, 1}); err != nil {
		t.Fatal(err)
	}
	if limits.acquire(now) {
		t.Fatal("unlimited RPM must not bypass shrinking concurrency")
	}
	limits.Release()
	if limits.acquire(now) {
		t.Fatal("existing call must drain")
	}
	limits.Release()
	if !limits.acquire(now) {
		t.Fatal("unlimited should remove only the rate gate")
	}
	limits.Release()
	if err := limits.Update(CallPolicy{"run", 3, "limited", 2, 2}); err != nil {
		t.Fatal(err)
	}
	if limits.acquire(now.Add(59 * time.Second)) {
		t.Fatal("policy changes must retain all three call attempts")
	}
	if !limits.acquire(now.Add(time.Minute)) {
		t.Fatal("real minute should replenish")
	}
	limits.Release()
	if err := limits.Update(CallPolicy{"run", 2, "unlimited", 0, 32}); err != nil {
		t.Fatal(err)
	}
	if limits.policy.Revision != 3 {
		t.Fatal("late policy must not replace active revision")
	}
}

func TestModelAttemptsAreCountedSeparatelyFromJobs(t *testing.T) {
	var limits CallLimits
	if err := limits.Update(CallPolicy{"run", 1, "limited", 3, 1}); err != nil {
		t.Fatal(err)
	}
	now := time.Unix(100, 0)
	// A prospective three-step job must acquire a model permit at every actual attempt.
	for step := 0; step < 3; step++ {
		if !limits.acquire(now) {
			t.Fatalf("step %d should fit", step)
		}
		limits.Release()
	}
	if limits.acquire(now) {
		t.Fatal("a fourth call must be throttled regardless of logical job identity")
	}
}

func TestRecoveryChargesUnknownLateExecutionAndDoesNotResetOnReconnect(t *testing.T) {
	var limits CallLimits
	now := time.Unix(100, 0)
	if err := limits.Seed([][]uint64{{100000, 2}}, now); err != nil {
		t.Fatal(err)
	}
	if err := limits.Update(CallPolicy{"run", 7, "limited", 2, 1}); err != nil {
		t.Fatal(err)
	}
	if limits.acquire(now.Add(time.Minute)) {
		t.Fatal("late prior execution remains charged after restart")
	}
	if err := limits.Seed(nil, now); err != nil {
		t.Fatal(err)
	}
	if limits.acquire(now.Add(time.Minute)) {
		t.Fatal("reconnect must not reset recovered history")
	}
	if err := limits.Update(CallPolicy{"new-run", 1, "limited", 2, 1}); err != nil {
		t.Fatal(err)
	}
	if limits.policy.Revision != 1 || limits.acquire(now) {
		t.Fatal("new run updates revision, not usage history")
	}
	if !limits.acquire(now.Add(105 * time.Second)) {
		t.Fatal("bounded uncertain execution eventually expires")
	}
	limits.Release()
}
