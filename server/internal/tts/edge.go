package tts

import (
	"context"
	"fmt"
	"time"

	edgetts "github.com/leopold7/edge-tts-go"
)

type Config struct {
	Voice          string
	Rate           string
	Volume         string
	Pitch          string
	ConnectTimeout time.Duration
	ReceiveTimeout time.Duration
}

type Result struct {
	MP3          []byte
	StartedAt    time.Time
	FirstAudioAt time.Time
	FinishedAt   time.Time
}

type Edge struct{ config Config }

func NewEdge(config Config) (*Edge, error) {
	if config.Voice == "" {
		config.Voice = "zh-CN-XiaoxiaoNeural"
	}
	if config.Rate == "" {
		config.Rate = "+0%"
	}
	if config.Volume == "" {
		config.Volume = "+0%"
	}
	if config.Pitch == "" {
		config.Pitch = "+0Hz"
	}
	if config.ConnectTimeout <= 0 {
		config.ConnectTimeout = 10 * time.Second
	}
	if config.ReceiveTimeout <= 0 {
		config.ReceiveTimeout = 60 * time.Second
	}
	return &Edge{config: config}, nil
}

func (e *Edge) Synthesize(ctx context.Context, text string) (Result, error) {
	if text == "" {
		return Result{}, fmt.Errorf("TTS text is empty")
	}
	config := edgetts.DefaultConfig()
	config.Voice = e.config.Voice
	config.Rate = e.config.Rate
	config.Volume = e.config.Volume
	config.Pitch = e.config.Pitch
	config.Boundary = edgetts.SentenceBoundary
	communicate, err := edgetts.NewCommunicate(text, config,
		edgetts.WithConnectTimeout(e.config.ConnectTimeout),
		edgetts.WithReceiveTimeout(e.config.ReceiveTimeout),
	)
	if err != nil {
		return Result{}, fmt.Errorf("create Edge-TTS request: %w", err)
	}
	result := Result{StartedAt: time.Now()}
	err = communicate.Stream(ctx, func(chunk edgetts.TTSChunk) error {
		if chunk.Type == edgetts.ChunkTypeAudio {
			if result.FirstAudioAt.IsZero() {
				result.FirstAudioAt = time.Now()
			}
			result.MP3 = append(result.MP3, chunk.Data...)
		}
		return nil
	})
	result.FinishedAt = time.Now()
	if err != nil {
		return Result{}, fmt.Errorf("Edge-TTS synthesis: %w", err)
	}
	if len(result.MP3) == 0 {
		return Result{}, fmt.Errorf("Edge-TTS returned no audio")
	}
	return result, nil
}
