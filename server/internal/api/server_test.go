package api

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/meoave/meoave/server/internal/device"
	"github.com/meoave/meoave/server/internal/llm"
	"github.com/meoave/meoave/server/internal/protocol"
	"github.com/meoave/meoave/server/internal/tts"
)

type fakeLLM struct {
	content string
	calls   int
}

func (f *fakeLLM) Complete(context.Context, string, string) (llm.CompletionResult, error) {
	f.calls++
	return llm.CompletionResult{Content: f.content, Model: "offline-test"}, nil
}

type fakeTTS struct{ calls int }

func (f *fakeTTS) Synthesize(ctx context.Context, text string) (tts.Result, error) {
	f.calls++
	if err := ctx.Err(); err != nil {
		return tts.Result{}, err
	}
	// Fifty mathematically silent MPEG-2 Layer III frames; no network or media asset.
	mp3 := make([]byte, 192*50)
	for i := 0; i < len(mp3); i += 192 {
		copy(mp3[i:], []byte{0xff, 0xf3, 0x84, 0xc0})
	}
	now := time.Now()
	return tts.Result{MP3: mp3, StartedAt: now, FirstAudioAt: now, FinishedAt: now}, nil
}

type fakePlayer struct {
	calls int
	fn    func(context.Context) error
}

func (p *fakePlayer) Play(ctx context.Context, s protocol.SentenceResponse) (device.Receipt, error) {
	p.calls++
	if p.fn != nil {
		if err := p.fn(ctx); err != nil {
			return device.Receipt{}, err
		}
	}
	return device.Receipt{Generation: s.Generation, SentenceID: s.SentenceID, Frames: s.Frames, PlayedFrames: s.Frames, ReportedFrames: s.Frames, Done: "played"}, nil
}

func testServer() (*Server, *fakeLLM, *fakeTTS, *fakePlayer) {
	s := New(nil, slog.New(slog.NewTextHandler(io.Discard, nil)))
	l := &fakeLLM{content: `{"sentences":[{"text":"你好呀。","expressions":[{"at_ms":0,"expression":"CURIOUS","intensity":0.5,"transition_ms":100}]},{"text":"我陪着你。","expressions":[{"at_ms":0,"expression":"HAPPY","intensity":0.7,"transition_ms":100}]}]}`}
	t := &fakeTTS{}
	p := &fakePlayer{}
	s.LLM, s.TTS, s.Device = l, t, p
	return s, l, t, p
}

func request(s *Server, path, body string) *httptest.ResponseRecorder {
	w := httptest.NewRecorder()
	s.Routes().ServeHTTP(w, httptest.NewRequest(http.MethodPost, path, strings.NewReader(body)))
	return w
}

func TestChatBothSentencesShareGenerationAndPlay(t *testing.T) {
	s, l, tts, p := testServer()
	w := request(s, "/v1/chat", `{"generation":7,"text":"你好","play":true}`)
	if w.Code != 200 {
		t.Fatalf("%d: %s", w.Code, w.Body.String())
	}
	if l.calls != 1 || tts.calls != 2 || p.calls != 2 {
		t.Fatalf("calls: %d/%d/%d", l.calls, tts.calls, p.calls)
	}
	var body struct {
		Sentences []protocol.SentenceResponse
		Device    []device.Receipt
		ASRTested bool `json:"asr_tested"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &body); err != nil {
		t.Fatal(err)
	}
	if len(body.Sentences) != 2 || body.Sentences[1].Generation != 7 || body.Sentences[1].SentenceID != 2 || body.ASRTested {
		t.Fatal("invalid turn contract")
	}
	if s.Sessions.Cancel(7) {
		t.Fatal("finished turn remained active")
	}
	if r := request(s, "/v1/chat", `{"generation":7,"text":"重复","play":true}`); r.Code != 409 {
		t.Fatal("reused turn accepted")
	}
}

func TestChatRejectsInvalidInputBeforeCloudOrDevice(t *testing.T) {
	for _, body := range []string{`{}`, `{"generation":1,"text":""}`, `{"generation":1,"text":"a","command":"RESET"}`, `{"generation":1,"text":"a"} {}`, `{"generation":1,"text":"` + strings.Repeat("a", 800) + `"}`} {
		s, l, tts, p := testServer()
		if w := request(s, "/v1/chat", body); w.Code != 400 {
			t.Fatalf("accepted %s", body)
		}
		if l.calls+tts.calls+p.calls != 0 {
			t.Fatal("invalid input caused a side effect")
		}
	}
}

func TestChatChecksAllChainsBeforePlayback(t *testing.T) {
	s, l, tts, p := testServer()
	l.content = strings.Replace(l.content, `"expression":"HAPPY"`, `"expression":"GPIO"`, 1)
	if w := request(s, "/v1/chat", `{"generation":1,"text":"你好","play":true}`); w.Code != 502 {
		t.Fatalf("%d %s", w.Code, w.Body.String())
	}
	if tts.calls+p.calls != 0 {
		t.Fatal("invalid model chain reached output")
	}
}

func TestMissingDeviceDoesNotCallCloud(t *testing.T) {
	s, l, _, _ := testServer()
	s.Device = nil
	if w := request(s, "/v1/chat", `{"generation":1,"text":"你好","play":true}`); w.Code != 503 {
		t.Fatal("missing device was not reported")
	}
	if l.calls != 0 {
		t.Fatal("cloud called without required device")
	}
	if w := request(s, "/v1/chat", `{"generation":2,"text":"你好","play":false}`); w.Code != 200 {
		t.Fatalf("server-only test failed: %s", w.Body.String())
	}
}

func TestCancelRemainsActiveDuringPlayback(t *testing.T) {
	s, _, _, p := testServer()
	p.fn = func(ctx context.Context) error {
		if !s.Sessions.Cancel(8) {
			t.Fatal("turn ended before playback")
		}
		<-ctx.Done()
		return ctx.Err()
	}
	w := request(s, "/v1/chat", `{"generation":8,"text":"你好","play":true}`)
	if w.Code != 502 || p.calls != 1 {
		t.Fatal("cancel continued later sentences")
	}
	if !strings.Contains(w.Body.String(), context.Canceled.Error()) {
		t.Fatal("cancel reason missing")
	}
}

func TestPlaybackFailureIsNotReportedAsSuccess(t *testing.T) {
	s, _, _, p := testServer()
	p.fn = func(context.Context) error { return errors.New("META_REJECTED") }
	w := request(s, "/v1/chat", `{"generation":1,"text":"你好","play":true}`)
	if w.Code != 502 || p.calls != 1 || !strings.Contains(w.Body.String(), "device_playback") {
		t.Fatal("false playback success")
	}
}

func TestPlayAlsoKeepsCancellationThroughDevicePlayback(t *testing.T) {
	s, _, _, p := testServer()
	p.fn = func(ctx context.Context) error {
		if !s.Sessions.Cancel(9) {
			t.Fatal("play finished the generation before playback")
		}
		return ctx.Err()
	}
	w := request(s, "/v1/play", `{"generation":9,"sentence_id":1,"text":"我在。","expressions":[{"at_ms":0,"expression":"CALM","intensity":0.5,"transition_ms":0}]}`)
	if w.Code != http.StatusRequestTimeout || p.calls != 1 {
		t.Fatalf("%d %s", w.Code, w.Body.String())
	}
}

func TestNilDependenciesAreTrulyUnconfigured(t *testing.T) {
	s := NewWithDevice(nil, slog.New(slog.NewTextHandler(io.Discard, nil)), nil)
	if s.Device != nil || s.TTS != nil {
		t.Fatal("typed nil leaked into readiness")
	}
	if r := request(s, "/v1/play", `{}`); r.Code != http.StatusServiceUnavailable {
		t.Fatal("unconfigured player not rejected")
	}
}
