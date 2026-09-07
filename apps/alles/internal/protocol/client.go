package protocol

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"net"
	"strconv"
	"sync"
	"sync/atomic"
	"time"
)

type response struct {
	ID     string          `json:"id"`
	Result json.RawMessage `json:"result"`
	Error  string          `json:"error"`
}
type Client struct {
	conn     net.Conn
	token    string
	mu       sync.Mutex
	write    sync.Mutex
	pending  map[string]chan response
	sequence atomic.Uint64
	done     chan struct{}
}

func Connect(ctx context.Context, address, token string) (*Client, error) {
	c, e := (&net.Dialer{}).DialContext(ctx, "tcp", address)
	if e != nil {
		return nil, e
	}
	p := &Client{conn: c, token: token, pending: map[string]chan response{}, done: make(chan struct{})}
	go p.read()
	return p, nil
}
func (c *Client) Close() { c.conn.Close() }
func (c *Client) read() {
	defer close(c.done)
	defer c.conn.Close()
	s := bufio.NewScanner(c.conn)
	s.Buffer(make([]byte, 4096), 65537)
	for s.Scan() {
		var r response
		if Strict(s.Bytes(), &r) != nil {
			return
		}
		c.mu.Lock()
		ch := c.pending[r.ID]
		c.mu.Unlock()
		if ch != nil {
			select {
			case ch <- r:
			default:
			}
		}
	}
}
func (c *Client) Call(ctx context.Context, op string, args any, dst any) error {
	id := strconv.FormatUint(c.sequence.Add(1), 10)
	ch := make(chan response, 1)
	c.mu.Lock()
	if len(c.pending) >= 16 {
		c.mu.Unlock()
		return errors.New("bridge request capacity")
	}
	c.pending[id] = ch
	c.mu.Unlock()
	defer func() { c.mu.Lock(); delete(c.pending, id); c.mu.Unlock() }()
	b, e := json.Marshal(map[string]any{"id": id, "token": c.token, "op": op, "args": args})
	if e != nil {
		return e
	}
	if len(b) > 65535 {
		return errors.New("bridge frame too large")
	}
	c.write.Lock()
	_ = c.conn.SetWriteDeadline(time.Now().Add(3 * time.Second))
	_, e = c.conn.Write(append(b, '\n'))
	c.write.Unlock()
	if e != nil {
		return e
	}
	var r response
	select {
	case r = <-ch:
	case <-ctx.Done():
		return ctx.Err()
	case <-c.done:
		// A complete response may already be buffered when the peer closes the socket.
		select {
		case r = <-ch:
		default:
			return errors.New("bridge disconnected")
		}
	}
	if r.Error != "" {
		return errors.New(r.Error)
	}
	return Strict(r.Result, dst)
}
