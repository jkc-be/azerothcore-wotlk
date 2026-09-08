// Package fixtureprovider supplies bounded deterministic responses for exclusive live acceptance trials.
// It has no model, bridge or world client; the ordinary interpreter still validates every response.
package fixtureprovider

import (
	"azerothcore/alles/internal/protocol"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"sync"
)

type Match struct {
	Path  []string        `json:"path"`
	Value json.RawMessage `json:"value"`
}

type Rule struct {
	Name     string          `json:"name"`
	Schema   string          `json:"schema"`
	Match    []Match         `json:"match"`
	Response json.RawMessage `json:"response,omitempty"`
	CopyFrom []string        `json:"copy_from,omitempty"`
	MaxUses  int             `json:"max_uses"`
}

type Config struct {
	Version     int    `json:"version"`
	Model       string `json:"model"`
	MaxRequests int    `json:"max_requests"`
	Rules       []Rule `json:"rules"`
}

type Server struct {
	config  Config
	model   string
	mu      sync.Mutex
	used    []int
	attempt int
	fault   bool
	commit  func([]byte) error
}

// commit must retain each attempt before its response is released. A journal failure permanently stops admission.
func New(data []byte, commit func([]byte) error) (*Server, error) {
	var config Config
	if err := protocol.Strict(data, &config); err != nil {
		return nil, err
	}
	if config.Version != 1 || len(config.Model) == 0 || len(config.Model) > 40 ||
		config.MaxRequests < 1 || config.MaxRequests > 100 || len(config.Rules) == 0 || len(config.Rules) > 64 || commit == nil {
		return nil, errors.New("invalid bounded fixture configuration")
	}
	names := map[string]bool{}
	for _, rule := range config.Rules {
		if rule.Name == "" || len(rule.Name) > 80 || names[rule.Name] || rule.MaxUses < 1 ||
			rule.MaxUses > config.MaxRequests || len(rule.Match) > 16 ||
			(rule.Schema != "memory_proposals" && rule.Schema != "conversation_response" && rule.Schema != "planning_response") ||
			(len(rule.Response) == 0) == (len(rule.CopyFrom) == 0) || len(rule.CopyFrom) > 8 {
			return nil, errors.New("invalid or duplicate fixture rule")
		}
		names[rule.Name] = true
		if len(rule.Response) != 0 {
			var object map[string]json.RawMessage
			if err := protocol.Strict(rule.Response, &object); err != nil || object == nil || len(rule.Response) > 8192 {
				return nil, errors.New("fixture response must be a bounded JSON object")
			}
		}
		for _, match := range rule.Match {
			var value any
			if len(match.Path) == 0 || len(match.Path) > 8 || protocol.Strict(match.Value, &value) != nil {
				return nil, errors.New("invalid fixture match")
			}
		}
	}
	digest := sha256.Sum256(data)
	return &Server{config: config, model: config.Model + "-" + hex.EncodeToString(digest[:]),
		used: make([]int, len(config.Rules)), commit: commit}, nil
}

func (s *Server) Model() string { return s.model }

// Paths traverse object keys only. No interpolation, expression evaluation or external lookup is performed.
func at(data json.RawMessage, path []string) (json.RawMessage, bool) {
	for _, key := range path {
		var object map[string]json.RawMessage
		if json.Unmarshal(data, &object) != nil {
			return nil, false
		}
		var ok bool
		data, ok = object[key]
		if !ok {
			return nil, false
		}
	}
	return data, true
}

func equal(left, right json.RawMessage) bool {
	var a, b any
	decode := func(data []byte, value *any) bool {
		d := json.NewDecoder(bytes.NewReader(data))
		d.UseNumber() // Preserve GUIDs beyond the exact range of float64.
		return d.Decode(value) == nil
	}
	if !decode(left, &a) || !decode(right, &b) {
		return false
	}
	x, _ := json.Marshal(a)
	y, _ := json.Marshal(b)
	return bytes.Equal(x, y)
}

func (s *Server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	if r.Method == http.MethodGet && r.URL.Path == "/models" {
		_ = json.NewEncoder(w).Encode(map[string]any{"data": []any{map[string]string{"id": s.model}}})
		return
	}
	if r.Method != http.MethodPost || r.URL.Path != "/chat/completions" {
		http.Error(w, "fixture route unavailable", http.StatusNotFound)
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.fault || s.attempt >= s.config.MaxRequests {
		http.Error(w, "fixture admission stopped", http.StatusTooManyRequests)
		return
	}
	s.attempt++
	data, err := io.ReadAll(io.LimitReader(r.Body, 65537))
	code, ruleName, response := http.StatusBadRequest, "", json.RawMessage(nil)
	if err == nil && len(data) <= 65536 {
		code, ruleName, response = s.selectResponse(data)
	}
	digest := sha256.Sum256(data)
	entry, _ := json.Marshal(map[string]any{"attempt": s.attempt, "rule": ruleName, "status": code,
		"request_sha256": hex.EncodeToString(digest[:])})
	if err := s.commit(append(entry, '\n')); err != nil {
		s.fault = true
		http.Error(w, "fixture journal failed", http.StatusServiceUnavailable)
		return
	}
	if code != http.StatusOK {
		http.Error(w, "fixture request rejected", code)
		return
	}
	_ = json.NewEncoder(w).Encode(map[string]any{"choices": []any{map[string]any{
		"message": map[string]string{"content": string(response)}, "finish_reason": "stop"}}})
}

func (s *Server) selectResponse(data []byte) (int, string, json.RawMessage) {
	var body map[string]json.RawMessage
	if protocol.Strict(data, &body) != nil {
		return http.StatusBadRequest, "", nil
	}
	var request struct {
		Model    string
		Stream   bool
		Messages []struct{ Role, Content string }
		Format   struct {
			Type   string
			Schema struct{ Name string } `json:"json_schema"`
		} `json:"response_format"`
	}
	if json.Unmarshal(data, &request) != nil || request.Model != s.model || request.Stream ||
		len(request.Messages) != 2 || request.Messages[0].Role != "system" || request.Messages[1].Role != "user" ||
		request.Format.Type != "json_schema" {
		return http.StatusBadRequest, "", nil
	}
	input := json.RawMessage(request.Messages[1].Content)
	var object map[string]json.RawMessage
	if protocol.Strict(input, &object) != nil || object == nil {
		return http.StatusBadRequest, "", nil
	}
	chosen := -1
	for i, rule := range s.config.Rules {
		if rule.Schema != request.Format.Schema.Name {
			continue
		}
		matches := true
		for _, match := range rule.Match {
			value, exists := at(input, match.Path)
			matches = matches && exists && equal(value, match.Value)
		}
		if !matches {
			continue
		}
		if chosen >= 0 {
			return http.StatusConflict, "ambiguous", nil
		}
		chosen = i
	}
	if chosen < 0 {
		return http.StatusConflict, "unmatched", nil
	}
	rule := s.config.Rules[chosen]
	if s.used[chosen] >= rule.MaxUses {
		return http.StatusConflict, rule.Name, nil
	}
	response := rule.Response
	if len(rule.CopyFrom) != 0 {
		var ok bool
		response, ok = at(input, rule.CopyFrom)
		if !ok {
			return http.StatusConflict, rule.Name, nil
		}
	}
	var result map[string]json.RawMessage
	if len(response) > 8192 || protocol.Strict(response, &result) != nil || result == nil {
		return http.StatusConflict, rule.Name, nil
	}
	s.used[chosen]++
	return http.StatusOK, rule.Name, response
}

func (s *Server) Description(baseURL string) string {
	data, _ := json.Marshal(map[string]any{"base_url": baseURL, "model": s.model, "max_requests": s.config.MaxRequests})
	return fmt.Sprintln(string(data))
}
