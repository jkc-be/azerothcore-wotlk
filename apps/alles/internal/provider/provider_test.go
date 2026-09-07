package provider

import (
	"azerothcore/alles/internal/protocol"
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func testJob() protocol.Job {
	return protocol.Job{Perceptions: json.RawMessage(`[]`),
		Memories: json.RawMessage(`[]`),
		Entities: json.RawMessage(`[]`),
		Places:   json.RawMessage(`[]`),
		Draft: protocol.Proposal{Memories: []protocol.Memory{{Supporting: []string{"p1"},
			Subject:    "e1",
			Source:     "e1",
			Text:       "A wolf died.",
			Confidence: 0.5,
			Salience:   0.7}}}}
}
func TestInterpretAndGrounding(t *testing.T) {
	for _, kind := range []string{"valid", "unknown-source", "duplicate", "truncated", "refusal"} {
		t.Run(kind, func(t *testing.T) {
			j := testJob()
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				if r.URL.Path != "/chat/completions" || r.Header.Get("Authorization") != "" {
					t.Error("request route/auth")
				}
				var body map[string]json.RawMessage
				_ = json.NewDecoder(r.Body).Decode(&body)
				if !strings.Contains(string(body["response_format"]), "json_schema") {
					t.Error("missing schema")
				}
				p := j.Draft
				if kind == "unknown-source" {
					p.Memories[0].Source = "e99"
				}
				b, _ := json.Marshal(p)
				content := string(b)
				if kind == "duplicate" {
					content = strings.Replace(content, `"kind":0`, `"kind":0,"kind":1`, 1)
				}
				if kind == "refusal" {
					content = "I cannot do that"
				}
				finish := "stop"
				if kind == "truncated" {
					finish = "length"
				}
				_ = json.NewEncoder(w).Encode(map[string]any{"choices": []any{map[string]any{"message": map[string]any{"content": content},
					"finish_reason": finish}}})
			}))
			defer server.Close()
			client := New(server.URL, "test")
			_, e := client.Interpret(context.Background(), testJob())
			if (e == nil) != (kind == "valid") {
				t.Fatalf("%s: %v", kind, e)
			}
		})
	}
}
func TestCancellation(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		select {
		case <-r.Context().Done():
		case <-time.After(2 * time.Second):
		}
	}))
	defer server.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Millisecond)
	defer cancel()
	start := time.Now()
	_, e := New(server.URL, "test").Interpret(ctx, testJob())
	if e == nil || time.Since(start) > time.Second {
		t.Fatal("cancellation failed")
	}
}

func TestConversationContractAndActionBounds(t *testing.T) {
	cases := []struct {
		name, content string
		valid         bool
	}{
		{"natural", `{"reply":true,"text":"What do you need a hand with?","action":"none"}`, true},
		{"follow", `{"reply":true,"text":"I'll accompany you for a little while.","action":"follow"}`, true},
		{"addressed-elsewhere", `{"reply":false,"text":"","action":"none"}`, true},
		{"arbitrary-command", `{"reply":true,"text":"Done","action":".modify money 999"}`, false},
		{"hidden-action", `{"reply":false,"text":"","action":"assist"}`, false},
		{"markup", `{"reply":true,"text":"|Hitem:1|hclick|h","action":"none"}`, false},
		{"newline", `{"reply":true,"text":"hi\nbye","action":"none"}`, false},
		{"duplicate", `{"reply":true,"reply":false,"text":"","action":"none"}`, false},
		{"unknown-field", `{"reply":true,"text":"hi","action":"none","command":"follow"}`, false},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				var body struct {
					Messages []struct{ Role, Content string }
					Format   json.RawMessage `json:"response_format"`
				}
				if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
					t.Error(err)
				}
				if len(body.Messages) != 2 || body.Messages[0].Content != ConversationContract ||
					!strings.Contains(body.Messages[1].Content, "could you lend a hand") ||
					!strings.Contains(string(body.Format), "conversation_response") {
					t.Error("wrong conversation contract")
				}
				_ = json.NewEncoder(w).Encode(map[string]any{"choices": []any{map[string]any{
					"message": map[string]any{"content": tc.content}, "finish_reason": "stop"}}})
			}))
			defer server.Close()
			_, _, err := New(server.URL, "test").Converse(context.Background(), protocol.Conversation{
				Context: json.RawMessage(`{"message":"could you lend a hand","canFollow":true}`),
			})
			if (err == nil) != tc.valid {
				t.Fatalf("valid=%t error=%v", tc.valid, err)
			}
		})
	}
}
