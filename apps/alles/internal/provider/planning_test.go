package provider

import (
	"azerothcore/alles/internal/protocol"
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func adviceJob() protocol.Planning {
	return protocol.Planning{Version: 1, Context: json.RawMessage(`{"purpose":"advice",
"question":{"text":"Where could I find work?"},
"capabilities":[{"name":"none","quest":false,"place":false,"person":false},
{"name":"remember_report","quest":false,"place":false,"person":false},
{"name":"investigate_report","quest":false,"place":true,"person":false},
{"name":"retry_quest","quest":true,"place":false,"person":false}],
"quests":[{"id":7}],"places":[{"id":12,"name":"Elwynn Forest","basis":"heard name"}],"people":[],
"evidence":[{"token":"reply","text":"I heard there may be work in Elwynn Forest.","source":"Traveler"}]}`)}
}

func TestPlanningGroundsEveryParameterAndUsesClosedResponse(t *testing.T) {
	valid := `{"version":1,"capability":"investigate_report","quest":0,"place":12,"person":null,"evidence":"reply","reason":"An uncertain lead to verify."}`
	cases := []struct {
		name, content string
		valid         bool
	}{
		{"heard-lead", valid, true},
		{"remember-warning", strings.ReplaceAll(strings.ReplaceAll(valid, `"investigate_report"`, `"remember_report"`), `"place":12`, `"place":0`), true},
		{"none", `{"version":1,"capability":"none","quest":0,"place":0,"person":null,"evidence":"","reason":"Unrelated chatter."}`, true},
		{"unknown-place", strings.ReplaceAll(valid, `"place":12`, `"place":999`), false},
		{"invented-capability", strings.ReplaceAll(valid, `"investigate_report"`, `"teleport"`), false},
		{"wrong-parameter", strings.ReplaceAll(valid, `"quest":0`, `"quest":7`), false},
		{"missing-place", strings.ReplaceAll(valid, `"place":12`, `"place":0`), false},
		{"null-place", strings.ReplaceAll(valid, `"place":12`, `"place":null`), false},
		{"unknown-evidence", strings.ReplaceAll(valid, `"reply"`, `"invented"`), false},
		{"empty-evidence", strings.ReplaceAll(valid, `"reply"`, `""`), false},
		{"unknown-person", strings.ReplaceAll(valid, `"person":null`, `"person":{"kind":0,"id":88}`), false},
		{"future-version", strings.ReplaceAll(valid, `"version":1`, `"version":2`), false},
		{"missing-field", strings.ReplaceAll(valid, `"quest":0,`, ``), false},
		{"extra-field", strings.ReplaceAll(valid, `"quest":0,`, `"quest":0,"command":"walk",`), false},
		{"duplicate", strings.ReplaceAll(valid, `"place":12`, `"place":12,"place":99`), false},
		{"too-long", strings.ReplaceAll(valid, `An uncertain lead to verify.`, strings.Repeat("x", 513)), false},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				var body map[string]json.RawMessage
				if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
					t.Fatal(err)
				}
				var format struct {
					Schema struct {
						Schema struct {
							Properties map[string]struct {
								Enum []json.RawMessage `json:"enum"`
							} `json:"properties"`
						} `json:"schema"`
					} `json:"json_schema"`
				}
				if err := json.Unmarshal(body["response_format"], &format); err != nil {
					t.Fatal(err)
				}
				places := format.Schema.Schema.Properties["place"].Enum
				if len(places) != 2 || string(places[1]) != "12" {
					t.Errorf("unexpected place enumeration: %s", places)
				}
				messages := string(body["messages"])
				if !strings.Contains(messages, "Never fill gaps") || !strings.Contains(messages, "warning or negative") {
					t.Error("missing private evidence/negative advice rules")
				}
				_ = json.NewEncoder(w).Encode(map[string]any{"choices": []any{map[string]any{
					"message": map[string]any{"content": tc.content}, "finish_reason": "stop"}}})
			}))
			defer server.Close()
			_, _, err := New(server.URL, "test").Plan(context.Background(), adviceJob())
			if (err == nil) != tc.valid {
				t.Fatalf("valid=%t err=%v", tc.valid, err)
			}
		})
	}
}

func TestPlanningRefusesInvalidWorldJobBeforeHTTP(t *testing.T) {
	for _, kind := range []string{"version", "oversized", "no-capabilities"} {
		t.Run(kind, func(t *testing.T) {
			job := adviceJob()
			switch kind {
			case "version":
				job.Version = 2
			case "oversized":
				job.Context = json.RawMessage(strings.Repeat(" ", 13000))
			case "no-capabilities":
				job.Context = json.RawMessage(`{}`)
			}
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { t.Error("unexpected HTTP request") }))
			defer server.Close()
			if _, _, err := New(server.URL, "test").Plan(context.Background(), job); err == nil {
				t.Fatal("accepted invalid job")
			}
		})
	}
}

func TestObjectiveDecisionRequiresASuppliedOptionAndAssociatedEvidence(t *testing.T) {
	const data = `{"purpose":"decision","capabilities":[
{"name":"none","quest":false,"place":false,"person":false},
{"name":"pursue_quest","quest":true,"place":false,"person":false},
{"name":"ask_quest_advice","quest":true,"place":false,"person":false}],
"quests":[{"id":7},{"id":8}],"places":[],"people":[],
"options":[{"capability":"pursue_quest","quest":7,"place":0,"person":null,"evidence":"objective-1"},
{"capability":"ask_quest_advice","quest":8,"place":0,"person":null,"evidence":"objective-2"}],
"evidence":[{"token":"objective-1"},{"token":"objective-2"},{"token":"report-1","quest":7},
{"token":"report-2","quest":8}]}`
	var state planningContext
	if err := json.Unmarshal([]byte(data), &state); err != nil {
		t.Fatal(err)
	}
	for _, tc := range []struct {
		name, capability, evidence string
		quest                      uint32
		valid                      bool
	}{
		{"own-work", "pursue_quest", "objective-1", 7, true},
		{"associated-report", "pursue_quest", "report-1", 7, true},
		{"ask-blocked-quest", "ask_quest_advice", "objective-2", 8, true},
		{"wrong-capability-for-quest", "ask_quest_advice", "objective-1", 7, false},
		{"different-quest-state", "pursue_quest", "objective-2", 7, false},
		{"unrelated-report", "pursue_quest", "report-2", 7, false},
		{"unsupported-retry", "pursue_quest", "objective-2", 8, false},
		{"keep-current", "none", "", 0, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			response := protocol.PlanningResponse{Version: 1, Capability: tc.capability, Quest: tc.quest,
				Evidence: tc.evidence, Reason: "Use the supplied own state."}
			raw, _ := json.Marshal(response)
			_, err := validatePlanning(string(raw), state)
			if (err == nil) != tc.valid {
				t.Fatalf("valid=%t err=%v", tc.valid, err)
			}
		})
	}
}

func TestRecruitmentUsesOnlyActualSpeakerAndSuppliedQuest(t *testing.T) {
	const data = `{"purpose":"recruitment","capabilities":[
{"name":"none","quest":false,"place":false,"person":false},
{"name":"invite_companion","quest":true,"place":false,"person":true}],
"quests":[{"id":7}],"places":[],"people":[{"kind":0,"id":2}],
"evidence":[{"token":"reply","text":"I can join you.","source":"Traveler"}]}`
	const valid = `{"version":1,"capability":"invite_companion","quest":7,"place":0,"person":{"kind":0,"id":2},"evidence":"reply","reason":"The speaker offered to join this quest."}`
	for _, tc := range []struct {
		name, reply string
		valid       bool
	}{
		{"actual-offer", valid, true},
		{"different-person", strings.ReplaceAll(valid, `"id":2`, `"id":3`), false},
		{"different-quest", strings.ReplaceAll(valid, `"quest":7`, `"quest":8`), false},
		{"not-invitation", strings.ReplaceAll(valid, `"invite_companion"`, `"accept_invitation"`), false},
		{"missing-evidence", strings.ReplaceAll(valid, `"evidence":"reply"`, `"evidence":""`), false},
		{"refusal", `{"version":1,"capability":"none","quest":0,"place":0,"person":null,"evidence":"","reason":"The reply declined the invitation."}`, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				var body map[string]json.RawMessage
				if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
					t.Fatal(err)
				}
				messages := string(body["messages"])
				if !strings.Contains(messages, "clear willingness") || !strings.Contains(messages, "invitation is not acceptance") {
					t.Error("missing recruitment consent rules")
				}
				_ = json.NewEncoder(w).Encode(map[string]any{"choices": []any{map[string]any{
					"message": map[string]any{"content": tc.reply}, "finish_reason": "stop"}}})
			}))
			defer server.Close()
			job := protocol.Planning{Version: 1, Context: json.RawMessage(data)}
			_, _, err := New(server.URL, "test").Plan(context.Background(), job)
			if (err == nil) != tc.valid {
				t.Fatalf("valid=%t err=%v", tc.valid, err)
			}
		})
	}
}
