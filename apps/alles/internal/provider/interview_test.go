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

func TestInterviewIsGroundedAndHasNoActionContract(t *testing.T) {
	for _, question := range []string{"Do you know Humane?", "Who are human*?", "What are you doing?", "Where next?", "What quests?"} {
		t.Run(question, func(t *testing.T) {
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				var body struct {
					Messages []struct{ Content string } `json:"messages"`
				}
				if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
					t.Error(err)
				}
				if !strings.Contains(body.Messages[0].Content, "History helps resolve") ||
					!strings.Contains(body.Messages[0].Content, "quests are your own current quest log") {
					t.Error("grounding contract missing")
				}
				_ = json.NewEncoder(w).Encode(map[string]any{"choices": []any{map[string]any{
					"message": map[string]any{"content": `{"text":"My current state is unavailable."}`}, "finish_reason": "stop"}}})
			}))
			defer server.Close()
			contextJSON, _ := json.Marshal(map[string]any{"message": question, "personalState": nil, "memories": []string{}})
			response, result, err := New(server.URL, "fixture").Interview(context.Background(), protocol.Interview{Context: contextJSON})
			if err != nil {
				t.Fatal(err)
			}
			if response.Text == "" || result.CallCount != 1 || result.UsageKnown {
				t.Fatal("invalid response or accounting")
			}
		})
	}
}
