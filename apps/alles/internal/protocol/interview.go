package protocol

import "encoding/json"

// Interview is read-only and has no gameplay action or memory proposal field.
type Interview struct {
	Token     string          `json:"jobToken"`
	Permit    string          `json:"permitId"`
	Remaining uint64          `json:"remainingMs"`
	Context   json.RawMessage `json:"context"`
}

type InterviewResponse struct {
	Text string `json:"text"`
}
