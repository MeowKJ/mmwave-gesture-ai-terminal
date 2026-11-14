package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/meoave/meoave/server/internal/api"
	"github.com/meoave/meoave/server/internal/device"
	"github.com/meoave/meoave/server/internal/tts"
)

func main() {
	listen := flag.String("listen", envOr("MEOAVE_LISTEN", "127.0.0.1:8787"), "HTTP listen address")
	voice := flag.String("voice", envOr("MEOAVE_TTS_VOICE", "zh-CN-XiaoxiaoNeural"), "Edge-TTS voice")
	rate := flag.String("rate", envOr("MEOAVE_TTS_RATE", "+0%"), "Edge-TTS rate")
	volume := flag.String("volume", envOr("MEOAVE_TTS_VOLUME", "+0%"), "Edge-TTS volume")
	pitch := flag.String("pitch", envOr("MEOAVE_TTS_PITCH", "+0Hz"), "Edge-TTS pitch")
	devicePort := flag.String("device-port", envOr("MEOAVE_DEVICE_PORT", ""), "native USB CDC port, for example COM10")
	deviceBaud := flag.Int("device-baud", envInt("MEOAVE_DEVICE_BAUD", 460800), "native USB CDC line rate")
	flag.Parse()

	log := slog.New(slog.NewTextHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelInfo}))
	ttsClient, err := tts.NewEdge(tts.Config{
		Voice: *voice, Rate: *rate, Volume: *volume, Pitch: *pitch,
		ConnectTimeout: 10 * time.Second, ReceiveTimeout: 60 * time.Second,
	})
	if err != nil {
		log.Error("configure TTS", "error", err)
		os.Exit(1)
	}
	var dev *device.Device
	if *devicePort != "" {
		dev, err = device.Open(*devicePort, *deviceBaud)
		if err != nil {
			log.Error("open device transport", "port", *devicePort, "error", err)
			os.Exit(1)
		}
		defer dev.Close()
		log.Info("Meoave native USB voice transport enabled", "port", *devicePort, "baud", *deviceBaud)
	}
	service := api.NewWithDevice(ttsClient, log, dev)
	server := &http.Server{Addr: *listen, Handler: service.Routes(), ReadHeaderTimeout: 5 * time.Second, WriteTimeout: 90 * time.Second}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	go func() {
		<-ctx.Done()
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = server.Shutdown(shutdownCtx)
	}()
	log.Info("Meoave Go server listening", "addr", *listen, "voice", *voice)
	if err := server.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
		log.Error("server stopped", "error", err)
		os.Exit(1)
	}
}

func envOr(key, fallback string) string {
	if value := os.Getenv(key); value != "" {
		return value
	}
	return fallback
}

func envInt(key string, fallback int) int {
	if value := os.Getenv(key); value != "" {
		var parsed int
		if _, err := fmt.Sscanf(value, "%d", &parsed); err == nil && parsed > 0 {
			return parsed
		}
	}
	return fallback
}
