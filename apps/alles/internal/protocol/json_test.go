package protocol

import (
	"encoding/json"
	"strings"
	"testing"
)

const valid = `{"memories":[{"operation":0,"supportingPerceptions":["p1"],"targetMemoryToken":"","subje` +
	`ctToken":"e1","sourceToken":"e1","placeToken":"l1","text":"A wolf was wounded; its cause` +
	` is unknown.","confidence":0.5,"salience":0.7,"kind":0}]}`

func TestProposalBoundary(t *testing.T) {
	for _, s := range []string{valid, "```json\n" + valid + "\n```", strings.Replace(valid, "wolf", "loup égaré", 1)} {
		if _, e := ParseProposal(s); e != nil {
			t.Fatal(e)
		}
	}
	for _, s := range []string{valid +
		valid,
		"Here is JSON: " +
			valid,
		"<think>x</think>" +
			valid,
		strings.Replace(valid,
			`"kind":0`,
			`"kind":0,"kind":1`,
			1),
		strings.Replace(valid,
			`"kind":0`,
			`"Kind":0`,
			1),
		strings.Replace(valid,
			`"kind":0`,
			`"kind":NaN`,
			1),
		strings.Replace(valid,
			"wolf",
			`\ud800`,
			1),
		strings.Replace(valid,
			`"confidence":0.5`,
			`"confidence":2`,
			1),
		valid[:len(valid)-2]} {
		if _, e := ParseProposal(s); e == nil {
			t.Fatalf("accepted %s", s)
		}
	}
}
func TestStrictIDsAndEscapedDuplicate(t *testing.T) {
	var v struct {
		ID uint64 `json:"id"`
	}
	if e := Strict([]byte(`{"id":18446744073709551615}`), &v); e != nil || v.ID != ^uint64(0) {
		t.Fatalf("%+v %v", v, e)
	}
	for _, s := range []string{`{"id":1,"\u0069d":2}`,
		`{"id":1.0}`,
		`{"id":-1}`,
		`{"id":18446744073709551616}`,
		strings.Repeat("[",
			18) + "0" + strings.Repeat("]",
			18)} {
		var v json.RawMessage
		if e := Strict([]byte(s), &v); e == nil && strings.HasPrefix(s, "[[") {
			t.Fatal("depth accepted")
		}
		var n struct {
			ID uint64 `json:"id"`
		}
		if e := Strict([]byte(s), &n); e == nil {
			t.Fatal(s)
		}
	}
}
