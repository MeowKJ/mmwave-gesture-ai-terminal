package protocol

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"strings"
)

const (
	Version       = 1
	SampleRate    = 48000
	Channels      = 1
	MaxTextBytes  = 768
	MaxFrames     = SampleRate * 30
	MaxCues       = 32
	MaxTransition = 2000
	MaxGeneration = ^uint32(0)
	MaxSentenceID = ^uint32(0)
)

// Cue is deliberately a closed data type. The device accepts semantic
// expression names only; it never accepts drawing code, GPIO operations or
// commands from the server.
type Cue struct {
	AtMS         uint32  `json:"at_ms"`
	Expression   string  `json:"expression"`
	Intensity    float64 `json:"intensity"`
	TransitionMS uint32  `json:"transition_ms"`
}

type SentenceRequest struct {
	Generation  uint32 `json:"generation"`
	SentenceID  uint32 `json:"sentence_id"`
	Text        string `json:"text"`
	Expressions []Cue  `json:"expressions"`
}

type SentenceResponse struct {
	V           int    `json:"v"`
	Generation  uint32 `json:"generation"`
	SentenceID  uint32 `json:"sentence_id"`
	SampleRate  int    `json:"sample_rate"`
	Channels    int    `json:"channels"`
	Format      string `json:"format"`
	Frames      int    `json:"frames"`
	Text        string `json:"text"`
	Expressions []Cue  `json:"expressions"`
	AudioBase64 string `json:"audio_base64"`
	SHA256      string `json:"sha256"`
	Timing      Timing `json:"timing"`
}

type Timing struct {
	TTSMS        int64 `json:"tts_ms"`
	DecodeMS     int64 `json:"decode_ms"`
	ResampleMS   int64 `json:"resample_ms"`
	TotalMS      int64 `json:"total_ms"`
	FirstAudioMS int64 `json:"first_audio_ms,omitempty"`
}

var allowedExpressions = map[string]struct{}{
	"CALM": {}, "HAPPY": {}, "CURIOUS": {}, "SLEEPY": {},
	"SURPRISED": {}, "WINK": {}, "SAD": {}, "EXCITED": {},
	"SHY": {}, "LOVE": {}, "PROUD": {}, "CONFUSED": {},
	"WORRIED": {}, "ANGRY": {}, "FOCUSED": {}, "RELIEVED": {},
}

func (c Cue) ValidateStructural() error {
	if _, ok := allowedExpressions[c.Expression]; !ok {
		return fmt.Errorf("expression %q is not in the device allow-list", c.Expression)
	}
	if math.IsNaN(c.Intensity) || math.IsInf(c.Intensity, 0) || c.Intensity < 0 || c.Intensity > 1 {
		return fmt.Errorf("intensity must be finite and between 0 and 1")
	}
	if c.TransitionMS > MaxTransition {
		return fmt.Errorf("transition_ms must be <= %d", MaxTransition)
	}
	return nil
}

func (r SentenceRequest) ValidateStructural() error {
	if r.Generation == 0 || r.Generation > MaxGeneration {
		return errors.New("generation must be non-zero")
	}
	if r.SentenceID == 0 || r.SentenceID > MaxSentenceID {
		return errors.New("sentence_id must be non-zero")
	}
	if strings.TrimSpace(r.Text) == "" {
		return errors.New("text must not be empty")
	}
	if len([]byte(r.Text)) > MaxTextBytes {
		return fmt.Errorf("text exceeds %d UTF-8 bytes", MaxTextBytes)
	}
	if len(r.Expressions) == 0 {
		return errors.New("every sentence must include a non-empty expression chain")
	}
	if len(r.Expressions) > MaxCues {
		return fmt.Errorf("expressions exceeds %d cues", MaxCues)
	}
	if r.Expressions[0].AtMS != 0 {
		return errors.New("the first expression cue must start at_ms=0")
	}
	var previous uint32
	for i, cue := range r.Expressions {
		if err := cue.ValidateStructural(); err != nil {
			return fmt.Errorf("cue %d: %w", i, err)
		}
		if i > 0 && cue.AtMS <= previous {
			return fmt.Errorf("cue %d is not strictly after the previous cue", i)
		}
		previous = cue.AtMS
	}
	return nil
}

// ValidateForFrames adds the checks that depend on the decoded audio length.
func (r SentenceRequest) ValidateForFrames(frames int) error {
	if err := r.ValidateStructural(); err != nil {
		return err
	}
	if frames <= 0 || frames > MaxFrames {
		return fmt.Errorf("audio frames must be between 1 and %d", MaxFrames)
	}
	for i, cue := range r.Expressions {
		atFrames := int64(cue.AtMS) * SampleRate / 1000
		endFrames := atFrames + int64(cue.TransitionMS)*SampleRate/1000
		if atFrames >= int64(frames) {
			return fmt.Errorf("cue %d starts after the audio ends", i)
		}
		if endFrames > int64(frames) {
			return fmt.Errorf("cue %d transition runs past the audio end", i)
		}
		if i+1 < len(r.Expressions) {
			nextFrames := int64(r.Expressions[i+1].AtMS) * SampleRate / 1000
			if endFrames > nextFrames {
				return fmt.Errorf("cue %d transition overlaps cue %d", i, i+1)
			}
		}
	}
	return nil
}

func (r SentenceRequest) Response(audioBase64, sha256 string, frames int, timing Timing) SentenceResponse {
	return SentenceResponse{
		V: Version, Generation: r.Generation, SentenceID: r.SentenceID,
		SampleRate: SampleRate, Channels: Channels, Format: "pcm_s16le",
		Frames: frames, Text: r.Text, Expressions: r.Expressions,
		AudioBase64: audioBase64, SHA256: sha256, Timing: timing,
	}
}

// DecodeStrict rejects unknown top-level fields, which catches accidental use
// of an older protocol instead of silently producing a device-incompatible
// sentence.
func DecodeStrict(data []byte, dst *SentenceRequest) error {
	dec := json.NewDecoder(strings.NewReader(string(data)))
	dec.DisallowUnknownFields()
	if err := dec.Decode(dst); err != nil {
		return err
	}
	var extra any
	if err := dec.Decode(&extra); err != io.EOF {
		if err == nil {
			return errors.New("request contains trailing JSON")
		}
		return fmt.Errorf("request has trailing data: %w", err)
	}
	return nil
}
