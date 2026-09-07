package protocol

import (
	"bytes"
	"encoding/json"
	"errors"
	"io"
	"strconv"
	"strings"
	"unicode/utf8"
)

// Strict rejects duplicate keys, invalid Unicode, excess nesting, and multiple documents.
func Strict(data []byte, dst any) error {
	if len(data) == 0 || len(data) > 65536 || !utf8.Valid(data) {
		return errors.New("JSON size or Unicode")
	}
	for i := 0; i < len(data); i++ {
		if data[i] != '\\' {
			continue
		}
		i++
		if i >= len(data) {
			break
		}
		if data[i] != 'u' {
			continue
		}
		if i+4 >= len(data) {
			return errors.New("Unicode escape")
		}
		n, e := strconv.ParseUint(string(data[i+1:i+5]), 16, 16)
		if e != nil {
			return e
		}
		i += 4
		if n >= 0xD800 && n <= 0xDBFF {
			if i+6 >= len(data) || string(data[i+1:i+3]) != "\\u" {
				return errors.New("unpaired surrogate")
			}
			low, e := strconv.ParseUint(string(data[i+3:i+7]), 16, 16)
			if e != nil || low < 0xDC00 || low > 0xDFFF {
				return errors.New("unpaired surrogate")
			}
			i += 6
		} else if n >= 0xDC00 && n <= 0xDFFF {
			return errors.New("unpaired surrogate")
		}
	}
	d := json.NewDecoder(bytes.NewReader(data))
	d.UseNumber()
	var walk func(int) error
	walk = func(depth int) error {
		if depth > 16 {
			return errors.New("JSON depth")
		}
		t, e := d.Token()
		if e != nil {
			return e
		}
		delim, ok := t.(json.Delim)
		if !ok {
			return nil
		}
		if delim != '{' && delim != '[' {
			return errors.New("JSON delimiter")
		}
		seen := map[string]bool{}
		for d.More() {
			if delim == '{' {
				k, e := d.Token()
				if e != nil {
					return e
				}
				s, ok := k.(string)
				if !ok || seen[s] {
					return errors.New("duplicate JSON key")
				}
				seen[s] = true
			}
			if e := walk(depth + 1); e != nil {
				return e
			}
		}
		_, e = d.Token()
		return e
	}
	if e := walk(0); e != nil {
		return e
	}
	if _, e := d.Token(); e != io.EOF {
		return errors.New("trailing JSON")
	}
	d = json.NewDecoder(bytes.NewReader(data))
	d.DisallowUnknownFields()
	return d.Decode(dst)
}

func Exact(data []byte, keys ...string) error {
	var raw map[string]json.RawMessage
	if e := Strict(data, &raw); e != nil {
		return e
	}
	if len(raw) != len(keys) {
		return errors.New("incorrect fields")
	}
	for _, key := range keys {
		if _, ok := raw[key]; !ok {
			return errors.New("missing field " + key)
		}
	}
	return nil
}

type Memory struct {
	Operation  uint64   `json:"operation"`
	Supporting []string `json:"supportingPerceptions"`
	Target     string   `json:"targetMemoryToken"`
	Subject    string   `json:"subjectToken"`
	Source     string   `json:"sourceToken"`
	Place      string   `json:"placeToken"`
	Text       string   `json:"text"`
	Confidence float64  `json:"confidence"`
	Salience   float64  `json:"salience"`
	Kind       uint64   `json:"kind"`
}
type Proposal struct {
	Memories []Memory `json:"memories"`
}

func ParseProposal(content string) (Proposal, error) {
	content = strings.TrimSpace(content)
	if strings.HasPrefix(content, "```json\n") && strings.HasSuffix(content, "\n```") {
		content = strings.TrimSpace(content[8 : len(content)-4])
	}
	var result Proposal
	if e := Exact([]byte(content), "memories"); e != nil {
		return result, e
	}
	if e := Strict([]byte(content), &result); e != nil {
		return result, e
	}
	var raw struct {
		Memories []json.RawMessage `json:"memories"`
	}
	_ = json.Unmarshal([]byte(content), &raw)
	if len(result.Memories) < 1 || len(result.Memories) > 4 {
		return result, errors.New("memory count")
	}
	for i, m := range result.Memories {
		if e := Exact(raw.Memories[i],
			"operation",
			"supportingPerceptions",
			"targetMemoryToken",
			"subjectToken",
			"sourceToken",
			"placeToken",
			"text",
			"confidence",
			"salience",
			"kind"); e != nil {
			return result, e
		}
		if m.Operation > 2 ||
			m.Kind > 5 ||
			len(m.Supporting) < 1 ||
			len(m.Supporting) > 8 ||
			utf8.RuneCountInString(m.Text) > 512 ||
			m.Text == "" ||
			strings.ContainsRune(m.Text,
				0) ||
			m.Confidence < 0 ||
			m.Confidence > 1 ||
			m.Salience < 0 ||
			m.Salience > 1 {
			return result, errors.New("invalid memory")
		}
	}
	return result, nil
}

type Job struct {
	Boot        string          `json:"bootEpoch"`
	Token       string          `json:"jobToken"`
	OwnerKind   uint64          `json:"ownerKind"`
	OwnerID     uint64          `json:"ownerId"`
	Generation  uint64          `json:"actorGeneration"`
	Worker      string          `json:"workerId"`
	Profile     string          `json:"profileFingerprint"`
	Lease       uint64          `json:"leaseGeneration"`
	Remaining   uint64          `json:"remainingMs"`
	Perceptions json.RawMessage `json:"perceptions"`
	Memories    json.RawMessage `json:"memories"`
	Entities    json.RawMessage `json:"entities"`
	Places      json.RawMessage `json:"places"`
	Draft       Proposal        `json:"draft"`
}

func (j Job) Envelope(p Proposal, permit string) map[string]any {
	return map[string]any{"bootEpoch": j.Boot,
		"jobToken":           j.Token,
		"ownerKind":          j.OwnerKind,
		"ownerId":            j.OwnerID,
		"actorGeneration":    j.Generation,
		"workerId":           j.Worker,
		"profileFingerprint": j.Profile,
		"leaseGeneration":    j.Lease,
		"permitId":           permit,
		"memories":           p.Memories}
}
