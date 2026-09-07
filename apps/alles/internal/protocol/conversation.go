package protocol

import "encoding/json"

// Context is trusted world-produced data, not a model-supplied command or database handle.
type Conversation struct {
	Token     string          `json:"jobToken"`
	Permit    string          `json:"permitId"`
	Remaining uint64          `json:"remainingMs"`
	Context   json.RawMessage `json:"context"`
}

type ConversationResponse struct {
	Reply  bool   `json:"reply"`
	Text   string `json:"text"`
	Action string `json:"action"`
}
