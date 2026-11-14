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
	"github.com/meoave/meoave/server/internal/protocol"
	"github.com/meoave/meoave/server/internal/session"
	"github.com/meoave/meoave/server/internal/tts"
)

type Server struct {
	TTS      *tts.Edge
	Sessions *session.Manager
	Device   *device.Device
	Log      *slog.Logger
}

func New(ttsClient *tts.Edge, log *slog.Logger) *Server {
	if log == nil {
		log = slog.Default()
	}
	return &Server{TTS: ttsClient, Sessions: session.NewManager(), Log: log}
}

func NewWithDevice(ttsClient *tts.Edge, log *slog.Logger, dev *device.Device) *Server {
	s := New(ttsClient, log)
	s.Device = dev
	return s
}

func (s *Server) Routes() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthz", s.healthz)
	mux.HandleFunc("POST /v1/synthesize", s.synthesize)
	mux.HandleFunc("POST /v1/play", s.play)
	mux.HandleFunc("POST /v1/cancel", s.cancel)
	return logging(mux, s.Log)
}

func (s *Server) healthz(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "service": "meoave-server", "generation": s.Sessions.Current()})
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
	response, err := s.synthesizeSentence(r.Context(), req)
	if err != nil {
		s.writeSynthesisError(w, r.Context(), err)
		return
	}
	receipt, err := s.Device.Play(r.Context(), response)
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
