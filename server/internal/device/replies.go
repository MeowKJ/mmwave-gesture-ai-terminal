package device

import (
	"context"
	"errors"
	"strings"
	"sync"
)

// CDC also carries periodic board telemetry. Reading only after VOICE OPEN
// lets the OS/device queues fill while a model or TTS request is in flight,
// and can cause the device to discard the OPEN acknowledgement. One reader
// therefore owns all serial reads for the complete lifetime of the service.
type replyReader struct {
	mu     sync.Mutex
	active bool
	err    error
	lines  chan string
	failed chan struct{}
	cancel context.CancelFunc
	done   chan struct{}
}

func newReplyReader(serial transport) *replyReader {
	ctx, cancel := context.WithCancel(context.Background())
	r := &replyReader{lines: make(chan string, 128), failed: make(chan struct{}), cancel: cancel, done: make(chan struct{})}
	go r.run(ctx, serial)
	return r
}

func (r *replyReader) fail(err error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.err == nil {
		r.err = err
		close(r.failed)
	}
}

func (r *replyReader) run(ctx context.Context, serial transport) {
	defer close(r.done)
	buf := make([]byte, 1024)
	line := make([]byte, 0, 512)
	oversized := false
	for ctx.Err() == nil {
		n, err := serial.ReadContext(ctx, buf)
		if err != nil {
			if ctx.Err() == nil {
				r.fail(err)
			}
			return
		}
		for _, b := range buf[:n] {
			if b != '\n' {
				if !oversized {
					if len(line) == 4096 {
						oversized = true
						line = line[:0]
					} else {
						line = append(line, b)
					}
				}
				continue
			}
			text := strings.TrimSpace(string(line))
			line = line[:0]
			if oversized {
				// Discard the entire oversized log record, not a tail that could
				// be mistaken for a machine-readable acknowledgement.
				oversized = false
				continue
			}
			if !strings.HasPrefix(text, "MEO:VOICE_") {
				continue
			}
			r.mu.Lock()
			if r.active {
				select {
				case r.lines <- text:
				default:
					// Never silently drop a completion and report success later.
					if r.err == nil {
						r.err = errors.New("device voice reply queue overflow")
						close(r.failed)
					}
				}
			}
			r.mu.Unlock()
		}
	}
}

func (r *replyReader) begin() error {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.err != nil {
		return r.err
	}
	for len(r.lines) > 0 {
		<-r.lines
	}
	r.active = true
	return nil
}

func (r *replyReader) end() {
	r.mu.Lock()
	r.active = false
	r.mu.Unlock()
}

func (r *replyReader) wait(ctx context.Context, predicate func(string) bool) (string, error) {
	for {
		select {
		case <-ctx.Done():
			return "", ctx.Err()
		case <-r.failed:
			r.mu.Lock()
			err := r.err
			r.mu.Unlock()
			return "", err
		case line := <-r.lines:
			if predicate(line) {
				return line, nil
			}
		}
	}
}

func (r *replyReader) close() {
	r.cancel()
	<-r.done
	r.fail(errors.New("device transport is closed"))
}
