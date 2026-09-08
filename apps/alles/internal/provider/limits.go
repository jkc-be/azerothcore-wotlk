package provider

import (
	"context"
	"errors"
	"sync"
	"time"
)

// CallPolicy is independent from prompts, actor evidence and job admission. Unlimited only removes RPM gating.
type CallPolicy struct {
	Run        string `json:"run"`
	Revision   uint64 `json:"revision"`
	Mode       string `json:"mode"`
	RPM        int    `json:"modelRpm"`
	Concurrent int    `json:"concurrentCalls"`
}

type callBucket struct {
	start, last time.Time
	count       uint64
}

type CallLimits struct {
	mu     sync.Mutex
	policy CallPolicy
	recent []callBucket
	active int
	seeded bool
}

// Reconnects in one process retain exact local usage. A new process conservatively recovers every durable
// reservation as a possibly late call (the job's maximum 45-second execution/admission fence).
func (l *CallLimits) Seed(buckets [][]uint64, now time.Time) error {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.seeded {
		return nil
	}
	if len(buckets) > 128 {
		return errors.New("reservation history exceeds bound")
	}
	for _, bucket := range buckets {
		if len(bucket) != 2 || bucket[0] > 1<<62 {
			return errors.New("invalid reservation history")
		}
		last := now.Add(time.UnixMilli(int64(bucket[0])).Add(45 * time.Second).Sub(now))
		if now.Sub(last) < time.Minute {
			l.recent = append(l.recent, callBucket{last, last, bucket[1]})
		}
	}
	l.seeded = true
	return nil
}

func (l *CallLimits) Update(p CallPolicy) error {
	if p.Revision == 0 || p.Concurrent < 1 || p.Concurrent > 32 || p.RPM < 0 || p.RPM > 100000 ||
		(p.Mode != "limited" && p.Mode != "unlimited" && p.Mode != "trial") || (p.Mode == "limited" && p.RPM == 0) {
		return errors.New("invalid model-call policy")
	}
	l.mu.Lock()
	defer l.mu.Unlock()
	if p.Run != l.policy.Run || p.Revision >= l.policy.Revision {
		l.policy = p
	}
	return nil
}

func (l *CallLimits) acquire(now time.Time) bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	cut := 0
	for cut < len(l.recent) && now.Sub(l.recent[cut].last) >= time.Minute {
		cut++
	}
	l.recent = l.recent[cut:]
	p := l.policy
	var used uint64
	for _, bucket := range l.recent {
		used += bucket.count
	}
	if p.Revision == 0 || l.active >= p.Concurrent || len(l.recent) >= 256 ||
		(p.Mode == "limited" && used >= uint64(p.RPM)) {
		return false
	}
	l.active++
	if len(l.recent) > 0 && !now.Before(l.recent[len(l.recent)-1].start) &&
		now.Sub(l.recent[len(l.recent)-1].start) < time.Second {
		bucket := &l.recent[len(l.recent)-1]
		bucket.count++
		bucket.last = now
	} else {
		l.recent = append(l.recent, callBucket{now, now, 1})
	}
	return true
}

func (l *CallLimits) Release() {
	l.mu.Lock()
	defer l.mu.Unlock()
	l.active--
}

func (l *CallLimits) Acquire(ctx context.Context, refresh func(context.Context) (CallPolicy, error)) error {
	for {
		policy, err := refresh(ctx)
		if err != nil {
			return err
		}
		if err := l.Update(policy); err != nil {
			return err
		}
		if l.acquire(time.Now()) {
			return nil
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(250 * time.Millisecond):
		}
	}
}
