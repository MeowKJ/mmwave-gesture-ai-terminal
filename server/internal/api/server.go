package api

import (
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"strings"
	"time"

	"github.com/meoave/meoave/server/internal/audio"
	"github.com/meoave/meoave/server/internal/device"
	"github.com/meoave/meoave/server/internal/dialogue"
	"github.com/meoave/meoave/server/internal/llm"
	"github.com/meoave/meoave/server/internal/protocol"
	"github.com/meoave/meoave/server/internal/session"
	"github.com/meoave/meoave/server/internal/tts"
)

type Synthesizer interface {
	Synthesize(context.Context, string) (tts.Result, error)
}

type Completer interface {
	Complete(context.Context, string, string) (llm.CompletionResult, error)
}

type Player interface {
	Play(context.Context, protocol.SentenceResponse) (device.Receipt, error)
}

type Server struct {
	TTS      Synthesizer
	LLM      Completer
	Sessions *session.Manager
	Device   Player
	Log      *slog.Logger
}

func New(ttsClient *tts.Edge, log *slog.Logger) *Server {
	if log == nil {
		log = slog.Default()
	}
	s := &Server{Sessions: session.NewManager(), Log: log}
	if ttsClient != nil {
		s.TTS = ttsClient
	}
	return s
}

func NewWithDevice(ttsClient *tts.Edge, log *slog.Logger, dev *device.Device) *Server {
	s := New(ttsClient, log)
	if dev != nil {
		s.Device = dev
	}
	return s
}

func (s *Server) Routes() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthz", s.healthz)
	mux.HandleFunc("POST /v1/synthesize", s.synthesize)
	mux.HandleFunc("POST /v1/play", s.play)
	mux.HandleFunc("POST /v1/chat", s.chat)
	mux.HandleFunc("POST /v1/cancel", s.cancel)
	return logging(mux, s.Log)
}

func (s *Server) healthz(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "service": "meoave-server", "generation": s.Sessions.Current(),
		"tts_configured": s.TTS != nil, "llm_configured": s.LLM != nil, "device_configured": s.Device != nil, "asr_configured": false})
}

func (s *Server) synthesize(w http.ResponseWriter, r *http.Request) {
	if s.TTS == nil {
		writeError(w, http.StatusServiceUnavailable, "tts is not configured")
		return
	}
	body, err := io.ReadAll(io.LimitReader(r.Body, 16*1024))
	if err != nil {
		writeError(w, http.StatusBadRequest, "read request: "+err.Error())
		return
	}
	var req protocol.SentenceRequest
	if err := protocol.DecodeStrict(body, &req); err != nil {
		writeError(w, http.StatusBadRequest, "invalid JSON: "+err.Error())
		return
	}
	if err := req.ValidateStructural(); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	response, err := s.synthesizeSentence(r.Context(), req)
	if err != nil {
		s.writeSynthesisError(w, r.Context(), err)
		return
	}
	writeJSON(w, http.StatusOK, response)
}

func (s *Server) synthesizeSentence(parent context.Context, req protocol.SentenceRequest) (protocol.SentenceResponse, error) {
	ctx, err := s.Sessions.Start(parent, req.Generation)
	if err != nil {
		return protocol.SentenceResponse{}, err
	}
	defer s.Sessions.Finish(req.Generation)
	return s.renderSentence(ctx, req)
}

// The caller owns the generation until synthesis AND playback have ended.
func (s *Server) renderSentence(ctx context.Context, req protocol.SentenceRequest) (protocol.SentenceResponse, error) {
	started := time.Now()
	ttsResult, err := s.TTS.Synthesize(ctx, req.Text)
	if err != nil {
		return protocol.SentenceResponse{}, err
	}
	decodeStarted := time.Now()
	decoded, err := audio.DecodeMP3(ttsResult.MP3)
	if err != nil {
		return protocol.SentenceResponse{}, err
	}
	decodeMS := time.Since(decodeStarted).Milliseconds()
	resampleStarted := time.Now()
	samples, err := audio.ResampleMonoS16(decoded.Samples, decoded.SampleRate, protocol.SampleRate)
	if err != nil {
		return protocol.SentenceResponse{}, err
	}
	resampleMS := time.Since(resampleStarted).Milliseconds()
	if err := req.ValidateForFrames(len(samples)); err != nil {
		return protocol.SentenceResponse{}, err
	}
	pcm := audio.EncodeS16LE(samples)
	hash := sha256.Sum256(pcm)
	firstAudioMS := int64(0)
	if !ttsResult.FirstAudioAt.IsZero() {
		firstAudioMS = ttsResult.FirstAudioAt.Sub(ttsResult.StartedAt).Milliseconds()
	}
	timing := protocol.Timing{TTSMS: ttsResult.FinishedAt.Sub(ttsResult.StartedAt).Milliseconds(), DecodeMS: decodeMS,
		ResampleMS: resampleMS, TotalMS: time.Since(started).Milliseconds(), FirstAudioMS: firstAudioMS}
	return req.Response(base64.StdEncoding.EncodeToString(pcm), hex.EncodeToString(hash[:]), len(samples), timing), nil
}

func (s *Server) writeSynthesisError(w http.ResponseWriter, ctx context.Context, err error) {
	if errors.Is(err, context.Canceled) || errors.Is(ctx.Err(), context.Canceled) {
		writeError(w, http.StatusRequestTimeout, "synthesis cancelled")
		return
	}
	status := http.StatusBadGateway
	if strings.Contains(err.Error(), "generation") || strings.Contains(err.Error(), "sentence") || strings.Contains(err.Error(), "cue") || strings.Contains(err.Error(), "expression") {
		status = http.StatusBadRequest
	}
	writeError(w, status, err.Error())
}

func (s *Server) play(w http.ResponseWriter, r *http.Request) {
	if s.Device == nil {
		writeError(w, http.StatusServiceUnavailable, "device transport is not configured; set MEOAVE_DEVICE_PORT")
		return
	}
	if s.TTS == nil {
		writeError(w, http.StatusServiceUnavailable, "tts is not configured")
		return
	}
	body, err := io.ReadAll(io.LimitReader(r.Body, 16*1024))
	if err != nil {
		writeError(w, http.StatusBadRequest, "read request: "+err.Error())
		return
	}
	var req protocol.SentenceRequest
	if err := protocol.DecodeStrict(body, &req); err != nil {
		writeError(w, http.StatusBadRequest, "invalid JSON: "+err.Error())
		return
	}
	if err := req.ValidateStructural(); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	ctx, err := s.Sessions.Start(r.Context(), req.Generation)
	if err != nil {
		s.writeSynthesisError(w, r.Context(), err)
		return
	}
	defer s.Sessions.Finish(req.Generation)
	response, err := s.renderSentence(ctx, req)
	if err != nil {
		s.writeSynthesisError(w, r.Context(), err)
		return
	}
	receipt, err := s.Device.Play(ctx, response)
	if err != nil {
		if errors.Is(err, context.Canceled) {
			writeError(w, http.StatusRequestTimeout, "device playback cancelled")
			return
		}
		writeError(w, http.StatusBadGateway, "device playback: "+err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "device": receipt, "sentence": response})
}

func (s *Server) chat(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Generation uint32 `json:"generation"`
		Text       string `json:"text"`
		Play       bool   `json:"play"`
	}
	r.Body = http.MaxBytesReader(w, r.Body, 4096)
	d := json.NewDecoder(r.Body)
	d.DisallowUnknownFields()
	if err := d.Decode(&req); err != nil {
		writeError(w, http.StatusBadRequest, "invalid chat request")
		return
	}
	var extra any
	if err := d.Decode(&extra); err != io.EOF || req.Generation == 0 || strings.TrimSpace(req.Text) == "" || len(req.Text) > protocol.MaxTextBytes {
		writeError(w, http.StatusBadRequest, "chat requires non-zero generation and 1..768 UTF-8 text bytes, without trailing JSON")
		return
	}
	if s.LLM == nil || s.TTS == nil || (req.Play && s.Device == nil) {
		writeError(w, http.StatusServiceUnavailable, "required LLM, TTS or device transport is not configured")
		return
	}
	ctx, err := s.Sessions.Start(r.Context(), req.Generation)
	if err != nil {
		writeError(w, http.StatusConflict, err.Error())
		return
	}
	defer s.Sessions.Finish(req.Generation)
	ctx, cancel := context.WithTimeout(ctx, 120*time.Second)
	defer cancel()
	started := time.Now()
	completion, err := s.LLM.Complete(ctx, dialogue.SystemPrompt, req.Text)
	llmMS := time.Since(started).Milliseconds()
	if err != nil {
		writeJSON(w, http.StatusBadGateway, map[string]any{"ok": false, "stage": "llm", "error": err.Error(),
			"generation": req.Generation, "llm_ms": llmMS, "total_ms": time.Since(started).Milliseconds()})
		return
	}
	plan, err := dialogue.Parse(completion.Content)
	if err != nil {
		writeJSON(w, http.StatusBadGateway, map[string]any{"ok": false, "stage": "expression_plan", "error": err.Error(),
			"generation": req.Generation, "model": completion.Model, "llm_ms": llmMS, "total_ms": time.Since(started).Milliseconds()})
		return
	}
	// Validate every chain/audio timeline before the first physical side effect.
	sentences := make([]protocol.SentenceResponse, 0, len(plan.Sentences))
	for i, sentence := range plan.Sentences {
		response, err := s.renderSentence(ctx, protocol.SentenceRequest{Generation: req.Generation, SentenceID: uint32(i + 1), Text: sentence.Text, Expressions: sentence.Expressions})
		if err != nil {
			writeJSON(w, http.StatusBadGateway, map[string]any{"ok": false, "stage": "synthesis", "error": err.Error(),
				"generation": req.Generation, "model": completion.Model, "llm_ms": llmMS, "total_ms": time.Since(started).Milliseconds()})
			return
		}
		sentences = append(sentences, response)
	}
	receipts := make([]device.Receipt, 0, len(sentences))
	if req.Play {
		for _, sentence := range sentences {
			receipt, err := s.Device.Play(ctx, sentence)
			if err != nil {
				writeJSON(w, http.StatusBadGateway, map[string]any{"ok": false, "stage": "device_playback", "error": err.Error(),
					"generation": req.Generation, "model": completion.Model, "llm_ms": llmMS, "total_ms": time.Since(started).Milliseconds(), "sentences": sentences, "device": receipts})
				return
			}
			receipts = append(receipts, receipt)
		}
	}
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "generation": req.Generation, "model": completion.Model,
		"llm_ms": llmMS, "total_ms": time.Since(started).Milliseconds(), "sentences": sentences, "device": receipts,
		"input": "text", "asr_tested": false})
}

func (s *Server) cancel(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Generation uint32 `json:"generation"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil && !errors.Is(err, io.EOF) {
		writeError(w, http.StatusBadRequest, "invalid JSON: "+err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"cancelled": s.Sessions.Cancel(req.Generation), "generation": req.Generation})
}

func writeJSON(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(value)
}

func writeError(w http.ResponseWriter, status int, message string) {
	writeJSON(w, status, map[string]any{"error": message})
}

func logging(next http.Handler, log *slog.Logger) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		started := time.Now()
		next.ServeHTTP(w, r)
		log.Info("http request", "method", r.Method, "path", r.URL.Path, "duration_ms", time.Since(started).Milliseconds())
	})
}
