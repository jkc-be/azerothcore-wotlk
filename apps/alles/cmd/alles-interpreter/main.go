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
	"syscall"
	"time"
)

type Config struct {
	BaseURL   string `json:"base_url"`
	Model     string `json:"model"`
	Bridge    string `json:"bridge"`
	TokenFile string `json:"token_file"`
}

func fingerprint(c Config) string {
	sum := sha256.Sum256([]byte(c.BaseURL +
		"\n" +
		c.Model + "\n" + provider.Contract + "\n" + provider.ConversationContract + "\n" + provider.PlanningContract +
		"\nslots=1;timeout=20;output=1024;chatOutput=512;inputBytes=12288;planningVersion=1;planningOutput=512;pilot=v3"))
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
			"permitId":         permit.ID,
			"outcome":          outcome,
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

func run(ctx context.Context, config Config, p *provider.Client, token string) error {
	c, e := protocol.Connect(ctx, config.Bridge, token)
	if e != nil {
		return e
	}
	defer c.Close()
	var hello struct {
		Worker  string `json:"workerId"`
		Profile string `json:"profile"`
		Used    uint64 `json:"usedRequests"`
	}
	if e = call(ctx,
		c,
		"worker_hello",
		map[string]any{"profile": fingerprint(config),
			"model":           config.Model,
			"maxInFlight":     1,
			"timeoutSeconds":  20,
			"planningVersion": 1},
		&hello); e != nil {
		return e
	}
	log.Printf("connected model=%s charged_requests=%d", config.Model, hello.Used)
	budgetLogged := false
	for ctx.Err() == nil {
		var r struct {
			Job          *protocol.Job          `json:"job"`
			Conversation *protocol.Conversation `json:"conversation"`
			Planning     *protocol.Planning     `json:"planning"`
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
		if r.Planning != nil {
			if e = plan(ctx, c, p, *r.Planning); e != nil {
				log.Printf("planning=%s failed: %v", r.Planning.Token, e)
			}
			continue
		}
		if r.Conversation != nil {
			if e = converse(ctx, c, p, *r.Conversation); e != nil {
				log.Printf("conversation=%s failed: %v", r.Conversation.Token, e)
			}
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
		if e = execute(ctx, c, p, *r.Job); e != nil {
			log.Printf("job=%s failed: %v", r.Job.Token, e)
		}
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
	if *printFingerprint {
		fmt.Println(fingerprint(config))
		return
	}
	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()
	p := provider.New(config.BaseURL, config.Model)
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
