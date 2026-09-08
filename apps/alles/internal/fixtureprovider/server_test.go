package fixtureprovider_test

import (
	"azerothcore/alles/internal/fixtureprovider"
	"azerothcore/alles/internal/protocol"
	"azerothcore/alles/internal/provider"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
)

func setup(t *testing.T, config fixtureprovider.Config) (*fixtureprovider.Server, *bytes.Buffer) {
	t.Helper()
	data, err := json.Marshal(config)
	if err != nil {
		t.Fatal(err)
	}
	journal := new(bytes.Buffer)
	server, err := fixtureprovider.New(data, func(entry []byte) error { _, err := journal.Write(entry); return err })
	if err != nil {
		t.Fatal(err)
	}
	return server, journal
}

func greeting() fixtureprovider.Rule {
	return fixtureprovider.Rule{Name: "greeting", Schema: "conversation_response", MaxUses: 1,
		Match:    []fixtureprovider.Match{{Path: []string{"bot"}, Value: json.RawMessage(`"Listener"`)}},
		Response: json.RawMessage(`{"reply":true,"text":"Hello there.","action":"none"}`)}
}

func request(t *testing.T, server *fixtureprovider.Server, input, schema string) *httptest.ResponseRecorder {
	t.Helper()
	data, err := json.Marshal(map[string]any{"model": server.Model(), "stream": false,
		"messages": []any{map[string]string{"role": "system", "content": "contract"},
			map[string]string{"role": "user", "content": input}},
		"response_format": map[string]any{"type": "json_schema", "json_schema": map[string]string{"name": schema}}})
	if err != nil {
		t.Fatal(err)
	}
	w := httptest.NewRecorder()
	server.ServeHTTP(w, httptest.NewRequest(http.MethodPost, "/chat/completions", bytes.NewReader(data)))
	return w
}

func TestNormalProviderConverseKeepsGroundingAndRecordsBeforeReply(t *testing.T) {
	server, journal := setup(t, fixtureprovider.Config{Version: 1, Model: "fixture", MaxRequests: 2,
		Rules: []fixtureprovider.Rule{greeting()}})
	httpServer := httptest.NewServer(server)
	defer httpServer.Close()
	client := provider.New(httpServer.URL, server.Model())
	if err := client.Ready(context.Background()); err != nil {
		t.Fatal(err)
	}
	response, _, err := client.Converse(context.Background(), protocol.Conversation{
		Context: json.RawMessage(`{"bot":"Listener","localActions":false}`)})
	if err != nil || !response.Reply || response.Text != "Hello there." || response.Action != "none" {
		t.Fatalf("ordinary provider response: %+v %v", response, err)
	}
	if !strings.Contains(journal.String(), `"rule":"greeting"`) || strings.Contains(journal.String(), "Listener") {
		t.Fatal("journal must identify the used rule without retaining raw input")
	}
	if request(t, server, `{"bot":"Listener"}`, "conversation_response").Code != http.StatusConflict {
		t.Fatal("exhausted rule replayed")
	}
	if request(t, server, `{"bot":"Listener"}`, "conversation_response").Code != http.StatusTooManyRequests {
		t.Fatal("fixture attempt cap was not enforced")
	}
}

func TestFixtureCannotBypassNormalActionValidation(t *testing.T) {
	rule := greeting()
	rule.Response = json.RawMessage(`{"reply":true,"text":"I will follow.","action":"follow"}`)
	server, _ := setup(t, fixtureprovider.Config{Version: 1, Model: "fixture", MaxRequests: 1,
		Rules: []fixtureprovider.Rule{rule}})
	httpServer := httptest.NewServer(server)
	defer httpServer.Close()
	_, _, err := provider.New(httpServer.URL, server.Model()).Converse(context.Background(), protocol.Conversation{
		Context: json.RawMessage(`{"bot":"Listener","localActions":false}`)})
	if err == nil {
		t.Fatal("fixture response bypassed the normal worker's unavailable-action rejection")
	}
}

func TestExactMatchingRejectsAmbiguityAndPreservesLargeIdentifiers(t *testing.T) {
	rule := greeting()
	rule.Match = []fixtureprovider.Match{{Path: []string{"actor", "id"}, Value: json.RawMessage(`9007199254740993`)}}
	server, _ := setup(t, fixtureprovider.Config{Version: 1, Model: "fixture", MaxRequests: 3,
		Rules: []fixtureprovider.Rule{rule}})
	if request(t, server, `{"actor":{"id":9007199254740992}}`, rule.Schema).Code != http.StatusConflict {
		t.Fatal("neighboring 64-bit identity matched")
	}
	if request(t, server, `{"actor":{"id":9007199254740993}}`, rule.Schema).Code != http.StatusOK {
		t.Fatal("exact 64-bit identity did not match")
	}
	other := rule
	other.Name = "overlap"
	server, _ = setup(t, fixtureprovider.Config{Version: 1, Model: "fixture", MaxRequests: 1,
		Rules: []fixtureprovider.Rule{rule, other}})
	if request(t, server, `{"actor":{"id":9007199254740993}}`, rule.Schema).Code != http.StatusConflict {
		t.Fatal("ambiguous fixture used order as a silent tie breaker")
	}
}

func TestMemoryDraftTravelsThroughTheOrdinaryProvider(t *testing.T) {
	server, _ := setup(t, fixtureprovider.Config{Version: 1, Model: "fixture", MaxRequests: 1,
		Rules: []fixtureprovider.Rule{{Name: "retain-draft", Schema: "memory_proposals", CopyFrom: []string{"draft"}, MaxUses: 1}}})
	httpServer := httptest.NewServer(server)
	defer httpServer.Close()
	job := protocol.Job{Perceptions: json.RawMessage(`[]`), Memories: json.RawMessage(`[]`),
		Entities: json.RawMessage(`[]`), Places: json.RawMessage(`[]`),
		Draft: protocol.Proposal{Memories: []protocol.Memory{{Supporting: []string{"p1"}, Subject: "e1", Source: "e1",
			Text: "A nearby traveler spoke.", Confidence: 0.5, Salience: 0.2}}}}
	result, err := provider.New(httpServer.URL, server.Model()).Interpret(context.Background(), job)
	if err != nil || len(result.Proposal.Memories) != 1 || result.Proposal.Memories[0].Text != job.Draft.Memories[0].Text {
		t.Fatalf("ordinary memory validation: %+v %v", result.Proposal, err)
	}
}

func TestPlanningFixtureStillRequiresIssuedPlaceAndEvidence(t *testing.T) {
	for _, place := range []int{12, 999} {
		response, _ := json.Marshal(map[string]any{"version": 1, "capability": "investigate_report",
			"quest": 0, "place": place, "person": nil, "evidence": "reply", "reason": "Investigate the supplied lead."})
		server, _ := setup(t, fixtureprovider.Config{Version: 1, Model: "fixture", MaxRequests: 1,
			Rules: []fixtureprovider.Rule{{Name: "heard-lead", Schema: "planning_response", MaxUses: 1,
				Match:    []fixtureprovider.Match{{Path: []string{"purpose"}, Value: json.RawMessage(`"advice"`)}},
				Response: response}}})
		httpServer := httptest.NewServer(server)
		job := protocol.Planning{Version: 1, Context: json.RawMessage(`{"purpose":"advice",
"capabilities":[{"name":"investigate_report","quest":false,"place":true,"person":false}],
"quests":[],"places":[{"id":12,"name":"Elwynn Forest"}],"people":[],
"evidence":[{"token":"reply","text":"There may be work in Elwynn Forest.","source":"Traveler"}]}`)}
		_, _, err := provider.New(httpServer.URL, server.Model()).Plan(context.Background(), job)
		httpServer.Close()
		if (err == nil) != (place == 12) {
			t.Fatalf("place %d: %v", place, err)
		}
	}
}

func TestJournalFailureStopsAdmissionAndConcurrentRequestsCannotReplayRule(t *testing.T) {
	config := fixtureprovider.Config{Version: 1, Model: "fixture", MaxRequests: 20, Rules: []fixtureprovider.Rule{greeting()}}
	data, _ := json.Marshal(config)
	server, err := fixtureprovider.New(data, func([]byte) error { return errors.New("disk failure") })
	if err != nil {
		t.Fatal(err)
	}
	if request(t, server, `{"bot":"Listener"}`, "conversation_response").Code != http.StatusServiceUnavailable ||
		request(t, server, `{"bot":"Listener"}`, "conversation_response").Code != http.StatusTooManyRequests {
		t.Fatal("response escaped after journal failure")
	}
	server, journal := setup(t, config)
	var group sync.WaitGroup
	statuses := make(chan int, 20)
	for range 20 {
		group.Go(func() { statuses <- request(t, server, `{"bot":"Listener"}`, "conversation_response").Code })
	}
	group.Wait()
	close(statuses)
	success := 0
	for code := range statuses {
		if code == http.StatusOK {
			success++
		}
	}
	if success != 1 || bytes.Count(journal.Bytes(), []byte{'\n'}) != 20 {
		t.Fatal("concurrent rule/budget accounting failed")
	}
}

func TestInvalidConfigurationsAndDuplicateInputFailClosed(t *testing.T) {
	for _, data := range []string{
		`{"version":1,"version":1}`, `{"version":2}`, `{"version":1,"model":"x","max_requests":101,"rules":[]}`,
	} {
		if _, err := fixtureprovider.New([]byte(data), func([]byte) error { return nil }); err == nil {
			t.Fatalf("accepted invalid fixture %s", data)
		}
	}
	server, _ := setup(t, fixtureprovider.Config{Version: 1, Model: "fixture", MaxRequests: 1,
		Rules: []fixtureprovider.Rule{greeting()}})
	if request(t, server, `{"bot":"Listener","bot":"SomeoneElse"}`, "conversation_response").Code != http.StatusBadRequest {
		t.Fatal("duplicate delivered context keys accepted")
	}
	changed := greeting()
	changed.Response = json.RawMessage(`{"reply":false,"text":"","action":"none"}`)
	other, _ := setup(t, fixtureprovider.Config{Version: 1, Model: "fixture", MaxRequests: 1,
		Rules: []fixtureprovider.Rule{changed}})
	if server.Model() == other.Model() {
		t.Fatal("changed rules retained the same worker profile model identity")
	}
}
