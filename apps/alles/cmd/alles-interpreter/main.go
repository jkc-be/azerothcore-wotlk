package main

import (
	"azerothcore/alles/internal/protocol"
	"azerothcore/alles/internal/provider"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"strings"
	"sync"
	"syscall"
	"time"
)

type Config struct {
	BackendProfile string `json:"backend_profile,omitempty"`
	BaseURL        string `json:"base_url"`
	Model          string `json:"model"`
	Bridge         string `json:"bridge"`
	TokenFile      string `json:"token_file"`
	APIKeyEnv      string `json:"api_key_env,omitempty"`
	Driver         string `json:"driver,omitempty"`
	MaxInFlight    int    `json:"max_in_flight,omitempty"`
}

func fingerprint(c Config) string {
	sum := sha256.Sum256([]byte(c.BaseURL +
		"\n" +
		c.Model + "\n" + provider.Contract + "\n" + provider.ConversationContract + "\n" + provider.PlanningContract +
		"\n" + provider.InterviewContract + "\ndriver=" + c.Driver + ";backendProfile=" + c.BackendProfile +
		";contract=2;maxCallsPerJob=1;timeout=20;output=1024;chatOutput=512;inputBytes=12288;planningVersion=1;planningOutput=512"))
	return hex.EncodeToString(sum[:])
}
func call(ctx context.Context, c *protocol.Client, op string, args, dst any) error {
	ctx, cancel := context.WithTimeout(ctx, 4*time.Second)
	defer cancel()
	return c.Call(ctx, op, args, dst)
}
func wait(ctx context.Context, d time.Duration) error {
	select {
	case <-time.After(d):
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}
func jobArgs(j protocol.Job) map[string]any {
	return map[string]any{"jobToken": j.Token, "leaseGeneration": j.Lease, "requestId": "attempt-1"}
}
func execute(ctx context.Context, c *protocol.Client, p *provider.Client, j protocol.Job) error {
	defer func() {
		releaseCtx, stop := context.WithTimeout(context.Background(), time.Second)
		defer stop()
		var r struct {
			OK bool `json:"ok"`
		}
		_ = call(releaseCtx, c, "release_job", jobArgs(j), &r)
	}()
	ctx, cancel := context.WithTimeout(ctx, time.Duration(j.Remaining)*time.Millisecond)
	defer cancel()
	done := make(chan struct{})
	heartbeatDone := make(chan struct{})
	defer func() { close(done); <-heartbeatDone }()
	go func() {
		defer close(heartbeatDone)
		ticker := time.NewTicker(5 * time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-done:
				return
			case <-ctx.Done():
				return
			case <-ticker.C:
				var r struct {
					OK bool `json:"ok"`
				}
				if call(ctx, c, "heartbeat", jobArgs(j), &r) != nil || !r.OK {
					cancel()
					return
				}
			}
		}
	}()
	var permit struct {
		Status string `json:"status"`
		ID     string `json:"permitId"`
	}
	for {
		if e := call(ctx, c, "begin_attempt", jobArgs(j), &permit); e != nil {
			return e
		}
		if permit.Status != "pending" {
			break
		}
		if e := wait(ctx, 100*time.Millisecond); e != nil {
			return e
		}
	}
	if permit.Status != "granted" {
		return errors.New("HTTP permit " + permit.Status)
	}
	attemptCtx, stop := context.WithTimeout(ctx, 20*time.Second)
	result, err := p.Interpret(attemptCtx, j)
	stop()
	outcome := "success"
	if err != nil {
		outcome = "failed"
	}
	var completed struct {
		Status string `json:"status"`
	}
	reportErr := call(ctx,
		c,
		"complete_attempt",
		map[string]any{"jobToken": j.Token,
			"permitId":  permit.ID,
			"outcome":   outcome,
			"callCount": result.CallCount, "usageKnown": result.UsageKnown,
			"promptTokens":     result.Usage.Prompt,
			"completionTokens": result.Usage.Completion,
			"latencyMs":        result.Latency.Milliseconds()},
		&completed)
	if err != nil {
		return err
	}
	if reportErr != nil {
		return reportErr
	}
	var applied struct {
		Status string `json:"status"`
	}
	if e := call(ctx,
		c,
		"submit_result",
		map[string]any{"proposal": j.Envelope(result.Proposal,
			permit.ID)},
		&applied); e != nil {
		return e
	}
	log.Printf("job=%s status=%s memories=%d prompt_tokens=%d completion_tokens=%d latency_ms=%d",
		j.Token,
		applied.Status,
		len(result.Proposal.Memories),
		result.Usage.Prompt,
		result.Usage.Completion,
		result.Latency.Milliseconds())
	return nil
}
func converse(ctx context.Context, c *protocol.Client, p *provider.Client, job protocol.Conversation) error {
	attemptCtx, stop := context.WithTimeout(ctx, min(20*time.Second, time.Duration(job.Remaining)*time.Millisecond))
	response, result, err := p.Converse(attemptCtx, job)
	stop()
	outcome := "success"
	if err != nil {
		outcome = "failed"
		response = protocol.ConversationResponse{Action: "none"}
	}
	var receipt struct {
		Status string `json:"status"`
	}
	reportErr := call(ctx, c, "submit_conversation", map[string]any{
		"jobToken": job.Token, "permitId": job.Permit, "response": response, "outcome": outcome,
		"callCount": result.CallCount, "usageKnown": result.UsageKnown,
		"promptTokens": result.Usage.Prompt, "completionTokens": result.Usage.Completion,
		"latencyMs": result.Latency.Milliseconds(),
	}, &receipt)
	log.Printf("conversation=%s status=%s action=%s reply=%t prompt_tokens=%d completion_tokens=%d latency_ms=%d",
		job.Token, receipt.Status, response.Action, response.Reply, result.Usage.Prompt,
		result.Usage.Completion, result.Latency.Milliseconds())
	if err != nil {
		return err
	}
	return reportErr
}

func plan(ctx context.Context, c *protocol.Client, p *provider.Client, job protocol.Planning) error {
	attemptCtx, stop := context.WithTimeout(ctx, min(20*time.Second, time.Duration(job.Remaining)*time.Millisecond))
	response, result, err := p.Plan(attemptCtx, job)
	stop()
	outcome := "success"
	if err != nil {
		outcome = "failed"
		response = protocol.PlanningResponse{}
	}
	var receipt struct {
		Status string `json:"status"`
	}
	reportErr := call(ctx, c, "submit_planning", map[string]any{
		"jobToken": job.Token, "permitId": job.Permit, "response": response, "outcome": outcome,
		"callCount": result.CallCount, "usageKnown": result.UsageKnown,
		"promptTokens": result.Usage.Prompt, "completionTokens": result.Usage.Completion,
		"latencyMs": result.Latency.Milliseconds(),
	}, &receipt)
	log.Printf("planning=%s status=%s capability=%s prompt_tokens=%d completion_tokens=%d latency_ms=%d",
		job.Token, receipt.Status, response.Capability, result.Usage.Prompt,
		result.Usage.Completion, result.Latency.Milliseconds())
	if err != nil {
		return err
	}
	return reportErr
}

func interview(ctx context.Context, c *protocol.Client, p *provider.Client, job protocol.Interview) error {
	attemptCtx, stop := context.WithTimeout(ctx, min(20*time.Second, time.Duration(job.Remaining)*time.Millisecond))
	response, result, err := p.Interview(attemptCtx, job)
	stop()
	outcome := "success"
	if err != nil {
		outcome = "failed"
	}
	var receipt struct {
		Status string `json:"status"`
	}
	reportErr := call(ctx, c, "submit_interview", map[string]any{
		"jobToken": job.Token, "permitId": job.Permit, "response": response, "outcome": outcome,
		"callCount": result.CallCount, "usageKnown": result.UsageKnown,
		"promptTokens": result.Usage.Prompt, "completionTokens": result.Usage.Completion,
		"latencyMs": result.Latency.Milliseconds(),
	}, &receipt)
	if err != nil {
		return err
	}
	return reportErr
}

func run(ctx context.Context, config Config, p *provider.Client, token string) error {
	c, e := protocol.Connect(ctx, config.Bridge, token)
	if e != nil {
		return e
	}
	defer c.Close()
	var hello struct {
		Worker             string          `json:"workerId"`
		Profile            string          `json:"profile"`
		Used               uint64          `json:"usedRequests"`
		Version            int             `json:"contractVersion"`
		Policy             json.RawMessage `json:"policy"`
		ReservationBuckets [][]uint64      `json:"reservationBuckets"`
	}
	if e = call(ctx,
		c,
		"worker_hello",
		map[string]any{"profile": fingerprint(config),
			"model":           config.Model,
			"maxInFlight":     config.MaxInFlight,
			"contractVersion": 2,
			"maxCallsPerJob":  1,
			"timeoutSeconds":  20,
			"planningVersion": 1},
		&hello); e != nil {
		return e
	}
	if err := p.Limits.Seed(hello.ReservationBuckets, time.Now()); err != nil {
		return err
	}
	p.Policy = func(ctx context.Context) (provider.CallPolicy, error) {
		var policy provider.CallPolicy
		err := call(ctx, c, "agent_policy", map[string]any{}, &policy)
		return policy, err
	}
	log.Printf("connected model=%s charged_requests=%d", config.Model, hello.Used)
	if hello.Version != 2 {
		return errors.New("agent contract version mismatch")
	}
	// One shared authenticated connection multiplexes a bounded set of executions. There is no local job queue.
	ctx, cancel := context.WithCancel(ctx)
	var running sync.WaitGroup
	defer func() { cancel(); running.Wait() }()
	slots := make(chan struct{}, config.MaxInFlight)
	launch := func(fn func() error) {
		slots <- struct{}{}
		running.Add(1)
		go func() {
			defer running.Done()
			defer func() { <-slots }()
			if err := fn(); err != nil {
				log.Printf("agent job failed: %v", err)
			}
		}()
	}
	budgetLogged := false
	for ctx.Err() == nil {
		if len(slots) == cap(slots) {
			if e = wait(ctx, 50*time.Millisecond); e != nil {
				return e
			}
			continue
		}
		var r struct {
			Job          *protocol.Job          `json:"job"`
			Conversation *protocol.Conversation `json:"conversation"`
			Planning     *protocol.Planning     `json:"planning"`
			Interview    *protocol.Interview    `json:"interview"`
			Retry        uint64                 `json:"retryMs"`
			Exhausted    bool                   `json:"budgetExhausted"`
			Fault        bool                   `json:"ledgerFault"`
		}
		if e = call(ctx, c, "next_jobs", map[string]any{"n": 1}, &r); e != nil {
			return e
		}
		if r.Fault {
			return errors.New("provider ledger fault")
		}
		if r.Interview != nil {
			launch(func() error { return interview(ctx, c, p, *r.Interview) })
			continue
		}
		if r.Planning != nil {
			launch(func() error { return plan(ctx, c, p, *r.Planning) })
			continue
		}
		if r.Conversation != nil {
			launch(func() error { return converse(ctx, c, p, *r.Conversation) })
			continue
		}
		if r.Exhausted {
			if !budgetLogged {
				log.Print("request allowance unavailable; polling for replenishment, gameplay continues")
				budgetLogged = true
			}
			if e = wait(ctx, 5*time.Second); e != nil {
				return e
			}
			continue
		}
		budgetLogged = false
		if r.Job == nil {
			if e = wait(ctx, 500*time.Millisecond); e != nil {
				return e
			}
			continue
		}
		launch(func() error { return execute(ctx, c, p, *r.Job) })
	}
	return ctx.Err()
}
func fixture(ctx context.Context, p *provider.Client, path, ledger string, max int, warm, conversation bool) error {
	b, e := os.ReadFile(path)
	if e != nil {
		return e
	}
	var job protocol.Job
	var chat protocol.Conversation
	if conversation {
		e = protocol.Strict(b, &chat)
	} else {
		e = protocol.Strict(b, &job)
	}
	if e != nil {
		return e
	}
	f, e := os.OpenFile(ledger, os.O_CREATE|os.O_RDWR|os.O_APPEND, 0600)
	if e != nil {
		return e
	}
	defer f.Close()
	if e = syscall.Flock(int(f.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); e != nil {
		return e
	}
	prior, e := os.ReadFile(ledger)
	if e != nil {
		return e
	}
	count := strings.Count(string(prior), "\n")
	if max < 1 || max > 100 || count >= max {
		return errors.New("fixture budget exhausted or invalid")
	}
	record,
		_ := json.Marshal(map[string]any{"attempt": count +
		1,
		"model":   p.Model,
		"fixture": path,
		"time":    time.Now().UTC()})
	if _, e = f.Write(append(record, '\n')); e != nil {
		return e
	}
	if e = f.Sync(); e != nil {
		return e
	}
	if warm {
		p.HTTP.Timeout = 120 * time.Second
	}
	if conversation {
		response, result, err := p.Converse(ctx, chat)
		_ = json.NewEncoder(os.Stdout).Encode(map[string]any{"response": response, "usage": result.Usage,
			"latencyMs": result.Latency.Milliseconds()})
		return err
	}
	result, e := p.Interpret(ctx, job)
	if e != nil {
		_ = json.NewEncoder(os.Stdout).Encode(result)
	}
	if e != nil {
		return e
	}
	return json.NewEncoder(os.Stdout).Encode(result)
}
func main() {
	path := flag.String("config", "", "worker JSON configuration")
	printFingerprint := flag.Bool("fingerprint", false, "print immutable profile fingerprint")
	fixturePath := flag.String("fixture", "", "interpret a bounded offline job fixture")
	conversationPath := flag.String("conversation-fixture", "", "interpret a bounded offline conversation fixture")
	ledger := flag.String("fixture-ledger", "", "durable fixture budget file")
	max := flag.Int("max-requests", 100, "fixture attempt cap (1-100)")
	warm := flag.Bool("warmup", false, "allow 120 seconds for fixture model loading")
	flag.Parse()
	if *conversationPath != "" {
		if *fixturePath != "" {
			log.Fatal("choose one fixture type")
		}
		*fixturePath = *conversationPath
	}
	b, e := os.ReadFile(*path)
	if e != nil {
		log.Fatal(e)
	}
	var config Config
	if e = protocol.Strict(b, &config); e != nil {
		log.Fatal(e)
	}
	if config.BaseURL == "" || config.Model == "" {
		log.Fatal("explicit base_url and model required")
	}
	if config.MaxInFlight == 0 {
		config.MaxInFlight = 32
	}
	if config.MaxInFlight < 1 || config.MaxInFlight > 32 {
		log.Fatal("max_in_flight must be 1-32")
	}
	if config.Driver != "" && config.Driver != "compatible" && config.Driver != "ai-sdk" {
		log.Fatal("driver must be compatible or ai-sdk")
	}
	if config.Driver == "ai-sdk" && len(config.BackendProfile) != 64 {
		log.Fatal("ai-sdk driver requires the adapter's pinned backend_profile")
	}
	if *printFingerprint {
		fmt.Println(fingerprint(config))
		return
	}
	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()
	p := provider.New(config.BaseURL, config.Model)
	p.Limits = &provider.CallLimits{}
	p.ExpectedBackendProfile = config.BackendProfile
	if config.APIKeyEnv != "" {
		p.APIKey = os.Getenv(config.APIKeyEnv)
		if p.APIKey == "" {
			log.Fatal("configured provider credential environment variable is empty")
		}
	}
	if e = p.Ready(ctx); e != nil {
		log.Fatal(e)
	}
	if *fixturePath != "" {
		if *ledger == "" {
			log.Fatal("fixture-ledger required")
		}
		if e = fixture(ctx, p, *fixturePath, *ledger, *max, *warm, *conversationPath != ""); e != nil {
			log.Fatal(e)
		}
		return
	}
	secret, e := os.ReadFile(config.TokenFile)
	if e != nil {
		log.Fatal(e)
	}
	token := strings.TrimSpace(string(secret))
	if len(token) < 32 {
		log.Fatal("bridge token too short")
	}
	for ctx.Err() == nil {
		if e = run(ctx, config, p, token); e != nil && ctx.Err() == nil {
			log.Printf("worker: %v", e)
		}
		_ = wait(ctx, 3*time.Second)
	}
}
