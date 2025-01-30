// Package main implements a simple HTTP load testing tool that can send concurrent GET or POST requests
package main

import (
	"context"
	"flag"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

// Configuration holds the application settings
type Config struct {
	URL           string
	Payload       string
	Threads       int
	Timeout       time.Duration
	RequestMethod string
}

// Statistics tracks the request statistics
type Statistics struct {
	RequestCount   uint64
	SuccessCount  uint64
	FailureCount  uint64
	ThrottleCount uint64
	ErrorCount    uint64
}

var (
	stats Statistics
	cfg   Config
)

// initFlags initializes and parses command-line flags
func initFlags() error {
	var getURL, postURL string

	flag.StringVar(&getURL, "g", "", "URL for GET request (e.g., -g 'http://example.com')")
	flag.StringVar(&postURL, "p", "", "URL for POST request (e.g., -p 'http://example.com')")
	flag.StringVar(&cfg.Payload, "d", "", "Data payload for POST request")
	flag.IntVar(&cfg.Threads, "t", 500, "Number of concurrent threads")
	flag.DurationVar(&cfg.Timeout, "timeout", 10*time.Second, "Request timeout duration")
	flag.Parse()

	// Validate flags
	if getURL != "" && postURL != "" {
		return fmt.Errorf("cannot specify both GET and POST requests")
	}

	if getURL == "" && postURL == "" {
		flag.Usage()
		return fmt.Errorf("must specify either GET (-g) or POST (-p) request")
	}

	if postURL != "" && cfg.Payload == "" {
		return fmt.Errorf("POST request requires data payload (-d)")
	}

	// Set the URL and method based on flags
	if getURL != "" {
		cfg.URL = getURL
		cfg.RequestMethod = "GET"
	} else {
		cfg.URL = postURL
		cfg.RequestMethod = "POST"
	}

	return nil
}

// createHTTPClient creates an HTTP client with timeout
func createHTTPClient() *http.Client {
	return &http.Client{
		Timeout: cfg.Timeout,
		Transport: &http.Transport{
			MaxIdleConns:        cfg.Threads,
			MaxIdleConnsPerHost: cfg.Threads,
			IdleConnTimeout:     30 * time.Second,
		},
	}
}

// sendRequest sends a single HTTP request and processes the response
func sendRequest(client *http.Client, ctx context.Context) error {
	var (
		req *http.Request
		err error
	)

	if cfg.RequestMethod == "POST" {
		req, err = http.NewRequestWithContext(ctx, "POST", cfg.URL,
			strings.NewReader(cfg.Payload))
		if err != nil {
			return fmt.Errorf("failed to create POST request: %w", err)
		}
		req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	} else {
		req, err = http.NewRequestWithContext(ctx, "GET", cfg.URL, nil)
		if err != nil {
			return fmt.Errorf("failed to create GET request: %w", err)
		}
	}

	resp, err := client.Do(req)
	if err != nil {
		atomic.AddUint64(&stats.ErrorCount, 1)
		return fmt.Errorf("request failed: %w", err)
	}
	defer resp.Body.Close()

	// Discard response body to reuse connection
	_, _ = io.Copy(io.Discard, resp.Body)

	// Update statistics based on response
	atomic.AddUint64(&stats.RequestCount, 1)
	switch resp.StatusCode {
	case http.StatusTooManyRequests:
		atomic.AddUint64(&stats.ThrottleCount, 1)
	case http.StatusInternalServerError:
		atomic.AddUint64(&stats.FailureCount, 1)
	case http.StatusOK:
		atomic.AddUint64(&stats.SuccessCount, 1)
	default:
		atomic.AddUint64(&stats.FailureCount, 1)
	}

	return nil
}

// printProgress prints the current statistics
func printProgress(ctx context.Context, done chan<- struct{}) {
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	defer close(done)

	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			fmt.Printf("\rRequests: %d | Success: %d | Failures: %d | Throttled: %d | Errors: %d",
				atomic.LoadUint64(&stats.RequestCount),
				atomic.LoadUint64(&stats.SuccessCount),
				atomic.LoadUint64(&stats.FailureCount),
				atomic.LoadUint64(&stats.ThrottleCount),
				atomic.LoadUint64(&stats.ErrorCount))
		}
	}
}

func main() {
	if err := initFlags(); err != nil {
		fmt.Fprintf(os.Stderr, "Error: %v\n", err)
		os.Exit(1)
	}

	// Setup cancellation context
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	// Handle interrupt signal
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, os.Interrupt)
	go func() {
		<-sigChan
		fmt.Println("\nReceived interrupt signal. Shutting down...")
		cancel()
	}()

	// Initialize HTTP client
	client := createHTTPClient()

	// Start progress reporting
	progressDone := make(chan struct{})
	go printProgress(ctx, progressDone)

	// Create worker pool
	var wg sync.WaitGroup
	wg.Add(cfg.Threads)

	// Launch workers
	for i := 0; i < cfg.Threads; i++ {
		go func() {
			defer wg.Done()
			for {
				select {
				case <-ctx.Done():
					return
				default:
					if err := sendRequest(client, ctx); err != nil {
						// Log error but continue processing
						fmt.Fprintf(os.Stderr, "\nError: %v\n", err)
					}
				}
			}
		}()
	}

	// Wait for completion or interruption
	wg.Wait()
	<-progressDone

	// Print final statistics
	fmt.Printf("\n\nFinal Statistics:\n")
	fmt.Printf("Total Requests: %d\n", stats.RequestCount)
	fmt.Printf("Successful: %d\n", stats.SuccessCount)
	fmt.Printf("Failed: %d\n", stats.FailureCount)
	fmt.Printf("Throttled: %d\n", stats.ThrottleCount)
	fmt.Printf("Errors: %d\n", stats.ErrorCount)
}

