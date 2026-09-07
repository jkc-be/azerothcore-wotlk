package protocol

import (
	"bufio"
	"context"
	"encoding/json"
	"net"
	"sync"
	"testing"
	"time"
)

func TestConcurrentCallsReceiveOutOfOrderResponses(t *testing.T) {
	listener, e := net.Listen("tcp", "127.0.0.1:0")
	if e != nil {
		t.Fatal(e)
	}
	defer listener.Close()
	go func() {
		conn, e := listener.Accept()
		if e != nil {
			return
		}
		defer conn.Close()
		scan := bufio.NewScanner(conn)
		var ids []string
		for len(ids) < 2 && scan.Scan() {
			var request struct {
				ID string `json:"id"`
			}
			_ = json.Unmarshal(scan.Bytes(), &request)
			ids = append(ids, request.ID)
		}
		if len(ids) == 2 {
			for i := 1; i >= 0; i-- {
				_ = json.NewEncoder(conn).Encode(map[string]any{"id": ids[i], "result": map[string]any{"value": ids[i]}})
			}
		}
	}()
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	client, e := Connect(ctx, listener.Addr().String(), "secret")
	if e != nil {
		t.Fatal(e)
	}
	defer client.Close()
	var wg sync.WaitGroup
	for i := 0; i < 2; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			var r struct {
				Value string `json:"value"`
			}
			if e := client.Call(ctx, "heartbeat", map[string]any{}, &r); e != nil {
				t.Error(e)
			} else if r.Value == "" {
				t.Error("missing response")
			}
		}()
	}
	wg.Wait()
}
