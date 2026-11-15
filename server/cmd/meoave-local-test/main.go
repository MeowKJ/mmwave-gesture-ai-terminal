// A single real HTTP turn. No fake model, reset, flashing, ASR claim or retry.
package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"time"

	"github.com/meoave/meoave/server/internal/device"
	"github.com/meoave/meoave/server/internal/protocol"
)

func run(server, text, reportPath string, play bool) (report map[string]any, resultErr error) {
	report = map[string]any{"ok": false, "started": time.Now().Format(time.RFC3339), "server": server, "input": "text", "asr_tested": false, "physical_screen_verified": false, "human_hearing_verified": false, "play_requested": play}
	defer func() {
		if resultErr != nil {
			report["error"] = resultErr.Error()
		}
		data, _ := json.MarshalIndent(report, "", "  ")
		if err := os.MkdirAll(filepath.Dir(reportPath), 0700); err != nil {
			resultErr = err
			return
		}
		file, err := os.OpenFile(reportPath, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
		if err != nil {
			resultErr = err
			return
		}
		defer file.Close()
		if _, err = file.Write(append(data, '\n')); err != nil {
			resultErr = err
		}
	}()
	client := &http.Client{Timeout: 150 * time.Second}
	resp, err := client.Get(server + "/healthz")
	if err != nil {
		return report, err
	}
	var health struct {
		OK         bool
		Generation uint32
		LLM        bool `json:"llm_configured"`
		TTS        bool `json:"tts_configured"`
		Device     bool `json:"device_configured"`
	}
	err = json.NewDecoder(resp.Body).Decode(&health)
	resp.Body.Close()
	if err != nil {
		return report, err
	}
	if !health.OK || !health.LLM || !health.TTS || (play && !health.Device) {
		return report, fmt.Errorf("required service is not configured")
	}
	gen := uint32(time.Now().Unix())
	if gen <= health.Generation {
		if health.Generation == ^uint32(0) {
			return report, fmt.Errorf("generation exhausted")
		}
		gen = health.Generation + 1
	}
	body, _ := json.Marshal(map[string]any{"generation": gen, "text": text, "play": play})
	started := time.Now()
	resp, err = client.Post(server+"/v1/chat", "application/json", bytes.NewReader(body))
	if err != nil {
		return report, err
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(io.LimitReader(resp.Body, 10*1024*1024))
	if err != nil {
		return report, err
	}
	var reply struct {
		OK        bool                        `json:"ok"`
		Error     string                      `json:"error"`
		Stage     string                      `json:"stage"`
		Model     string                      `json:"model"`
		LLMMS     int64                       `json:"llm_ms"`
		TotalMS   int64                       `json:"total_ms"`
		Sentences []protocol.SentenceResponse `json:"sentences"`
		Device    []device.Receipt            `json:"device"`
	}
	if err = json.Unmarshal(data, &reply); err != nil {
		return report, fmt.Errorf("HTTP %d invalid response: %w", resp.StatusCode, err)
	}
	report["http_status"], report["model"], report["llm_ms"], report["elapsed_ms"], report["stage"] = resp.StatusCode, reply.Model, reply.LLMMS, time.Since(started).Milliseconds(), reply.Stage
	items := []map[string]any{}
	for i, sentence := range reply.Sentences {
		pcm, err := base64.StdEncoding.DecodeString(sentence.AudioBase64)
		if err != nil {
			return report, err
		}
		hash := sha256.Sum256(pcm)
		request := protocol.SentenceRequest{Generation: sentence.Generation, SentenceID: sentence.SentenceID, Text: sentence.Text, Expressions: sentence.Expressions}
		if sentence.Generation != gen || sentence.SentenceID != uint32(i+1) || len(pcm) != sentence.Frames*2 || hex.EncodeToString(hash[:]) != sentence.SHA256 || sentence.SampleRate != 48000 || sentence.Channels != 1 || sentence.Format != "pcm_s16le" {
			return report, fmt.Errorf("invalid PCM contract for sentence %d", i+1)
		}
		if err = request.ValidateForFrames(sentence.Frames); err != nil {
			return report, err
		}
		items = append(items, map[string]any{"text": sentence.Text, "sentence_id": sentence.SentenceID, "generation": gen, "expressions": sentence.Expressions, "frames": sentence.Frames, "duration_ms": sentence.Frames / 48, "sha256": sentence.SHA256, "timing": sentence.Timing})
	}
	report["sentences"], report["device"] = items, reply.Device
	if resp.StatusCode != 200 || !reply.OK {
		return report, fmt.Errorf("HTTP %d: %s", resp.StatusCode, reply.Error)
	}
	if len(items) == 0 || len(items) > 2 || (play && len(reply.Device) != len(items)) {
		return report, fmt.Errorf("incomplete turn")
	}
	for i, receipt := range reply.Device {
		if receipt.Generation != gen || receipt.SentenceID != uint32(i+1) || receipt.PlayedFrames != reply.Sentences[i].Frames || receipt.ReportedFrames != receipt.PlayedFrames || receipt.Error != 0 || receipt.Done != "played" {
			return report, fmt.Errorf("invalid hardware completion %d", i+1)
		}
	}
	report["ok"] = true
	return report, nil
}

func main() {
	server := flag.String("server", "http://127.0.0.1:8787", "local server URL")
	text := flag.String("text", "猫猫，我今天有点累，请简短地安慰我。", "one test text message")
	play := flag.Bool("play", true, "send the real reply to the P4, not PC speakers")
	reportPath := flag.String("report", "", "new report JSON path (never overwritten)")
	flag.Parse()
	if *reportPath == "" {
		fmt.Fprintln(os.Stderr, "-report is required")
		os.Exit(2)
	}
	if _, err := os.Stat(*reportPath); !os.IsNotExist(err) {
		fmt.Fprintln(os.Stderr, "report exists or cannot be accessed")
		os.Exit(2)
	}
	report, err := run(*server, *text, *reportPath, *play)
	data, _ := json.MarshalIndent(report, "", "  ")
	fmt.Println(string(data))
	fmt.Println("report:", *reportPath)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
