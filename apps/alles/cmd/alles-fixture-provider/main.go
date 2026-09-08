// alles-fixture-provider is an opt-in loopback service for deterministic acceptance trials, never a model proxy.
package main

import (
	"azerothcore/alles/internal/fixtureprovider"
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"
)

func run() error {
	fixture := flag.String("fixture", "", "bounded deterministic rule JSON")
	journal := flag.String("journal", "", "new exclusive attempt journal; never reuse a previous run")
	listen := flag.String("listen", "127.0.0.1:11435", "IPv4 loopback address")
	describe := flag.Bool("describe", false, "validate and print provider identity without opening a listener or journal")
	flag.Parse()
	host, _, err := net.SplitHostPort(*listen)
	if err != nil || host != "127.0.0.1" || *fixture == "" {
		return errors.New("fixture and numeric IPv4 loopback listen address required")
	}
	input, err := os.Open(*fixture)
	if err != nil {
		return err
	}
	data, err := io.ReadAll(io.LimitReader(input, 65537))
	_ = input.Close()
	if err != nil {
		return err
	}
	var output *os.File
	handler, err := fixtureprovider.New(data, func(entry []byte) error {
		if output == nil {
			return errors.New("journal unavailable")
		}
		if n, err := output.Write(entry); err != nil {
			return err
		} else if n != len(entry) {
			return io.ErrShortWrite
		}
		return output.Sync()
	})
	if err != nil {
		return err
	}
	if *describe {
		fmt.Print(handler.Description("http://" + *listen))
		return nil
	}
	if *journal == "" {
		return errors.New("a new attempt journal is required")
	}
	listener, err := net.Listen("tcp4", *listen)
	if err != nil {
		return err
	}
	defer listener.Close()
	output, err = os.OpenFile(*journal, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return err
	}
	defer output.Close()
	server := &http.Server{Handler: handler, ReadHeaderTimeout: 2 * time.Second,
		ReadTimeout: 5 * time.Second, WriteTimeout: 5 * time.Second, IdleTimeout: 10 * time.Second}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	done := make(chan error, 1)
	go func() { done <- server.Serve(listener) }()
	fmt.Print(handler.Description("http://" + listener.Addr().String()))
	select {
	case err := <-done:
		return err
	case <-ctx.Done():
		shutdown, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		if err := server.Shutdown(shutdown); err != nil {
			_ = server.Close()
			return err
		}
		if err := <-done; !errors.Is(err, http.ErrServerClosed) {
			return err
		}
		return nil
	}
}

func main() {
	if err := run(); err != nil {
		log.Fatal(err)
	}
}
