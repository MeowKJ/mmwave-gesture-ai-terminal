package device

import (
	"context"
	"errors"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

type testTransport struct {
	reads     chan []byte
	readError error
	consumed  atomic.Int32
}

func (s *testTransport) ReadContext(ctx context.Context, dst []byte) (int, error) {
	if s.readError != nil {
		return 0, s.readError
	}
	select {
	case <-ctx.Done():
		return 0, ctx.Err()
	case data := <-s.reads:
		s.consumed.Add(1)
		return copy(dst, data), nil
	}
}
func (*testTransport) WriteContext(context.Context, []byte) error { return nil }
func (*testTransport) Close() error                               { return nil }

func TestReaderDrainsIdleTelemetry(t *testing.T) {
	s := &testTransport{reads: make(chan []byte, 256)}
	r := newReplyReader(s)
	defer r.close()
	for i := 0; i < 200; i++ {
		s.reads <- []byte("FLS2:STATUS ready=1\nMEO:VOICE_IDLE token=old\n")
	}
	deadline := time.Now().Add(time.Second)
	for s.consumed.Load() != 200 && time.Now().Before(deadline) {
		time.Sleep(time.Millisecond)
	}
	if s.consumed.Load() != 200 {
		t.Fatal("idle CDC was not drained")
	}
	if err := r.begin(); err != nil {
		t.Fatal(err)
	}
	s.reads <- []byte("MEO:VOICE_OPEN token=new accepted=1\n")
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	line, err := r.wait(ctx, func(s string) bool { return strings.Contains(s, "token=new") })
	if err != nil || !strings.Contains(line, "accepted=1") {
		t.Fatalf("reply=%q error=%v", line, err)
	}
}

func TestReaderKeepsCoalescedAndFragmentedReplies(t *testing.T) {
	s := &testTransport{reads: make(chan []byte, 8)}
	r := newReplyReader(s)
	defer r.close()
	if err := r.begin(); err != nil {
		t.Fatal(err)
	}
	s.reads <- []byte("FLS2:STATUS ready=1\nMEO:VOI")
	s.reads <- []byte("CE_PCM_ACCEPTED token=a seq=1\r\nMEO:VOICE_SENTENCE_DONE generation=1 sentence=1 played=48 frames=48 error=0\n")
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	for _, want := range []string{"PCM_ACCEPTED", "SENTENCE_DONE"} {
		line, err := r.wait(ctx, func(s string) bool { return strings.Contains(s, want) })
		if err != nil || !strings.Contains(line, want) {
			t.Fatalf("%s: %q %v", want, line, err)
		}
	}
}

func TestReaderDiscardsEntireOversizedRecord(t *testing.T) {
	s := &testTransport{reads: make(chan []byte, 16)}
	r := newReplyReader(s)
	defer r.close()
	_ = r.begin()
	for i := 0; i < 5; i++ {
		s.reads <- []byte(strings.Repeat("x", 1000))
	}
	s.reads <- []byte("MEO:VOICE_OPEN token=forged accepted=1\nMEO:VOICE_OPEN token=real accepted=1\n")
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	line, err := r.wait(ctx, func(s string) bool { return strings.Contains(s, "VOICE_OPEN") })
	if err != nil || !strings.Contains(line, "token=real") {
		t.Fatalf("oversized tail admitted: %q %v", line, err)
	}
}

func TestReaderPropagatesReadError(t *testing.T) {
	want := errors.New("disconnected")
	r := newReplyReader(&testTransport{readError: want})
	defer r.close()
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	_, err := r.wait(ctx, func(string) bool { return true })
	if !errors.Is(err, want) {
		t.Fatalf("got %v", err)
	}
}

func TestReaderFailsClosedOnVoiceOverflow(t *testing.T) {
	s := &testTransport{reads: make(chan []byte, 256)}
	r := newReplyReader(s)
	defer r.close()
	_ = r.begin()
	for i := 0; i < 140; i++ {
		s.reads <- []byte("MEO:VOICE_NACK token=x reason=ERROR\n")
	}
	select {
	case <-r.failed:
	case <-time.After(time.Second):
		t.Fatal("voice overflow not reported")
	}
	if err := r.begin(); err == nil || !strings.Contains(err.Error(), "overflow") {
		t.Fatalf("got %v", err)
	}
}
