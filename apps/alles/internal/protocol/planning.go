package protocol

import "encoding/json"

// The world owns the immutable job and checks its actor/objective fences again at application.
type Planning struct {
	Token     string          `json:"jobToken"`
	Permit    string          `json:"permitId"`
	Remaining uint64          `json:"remainingMs"`
	Version   uint32          `json:"contractVersion"`
	Context   json.RawMessage `json:"context"`
}

type PlanningActor struct {
	Kind uint8  `json:"kind"`
	ID   uint64 `json:"id"`
}

type PlanningResponse struct {
	Version    uint32         `json:"version"`
	Capability string         `json:"capability"`
	Quest      uint32         `json:"quest"`
	Place      uint32         `json:"place"`
	Person     *PlanningActor `json:"person"`
	Evidence   string         `json:"evidence"`
	Reason     string         `json:"reason"`
}
