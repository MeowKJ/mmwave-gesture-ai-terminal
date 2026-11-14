package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"strings"
	"time"

	"github.com/meoave/meoave/server/internal/audio"
	"github.com/meoave/meoave/server/internal/llm"
	"github.com/meoave/meoave/server/internal/protocol"
	"github.com/meoave/meoave/server/internal/tts"
)

type reply struct {
	Sentences []struct {
		Text        string         `json:"text"`
		Expressions []protocol.Cue `json:"expressions"`
	} `json:"sentences"`
}

func main() {
	base := flag.String("base-url", envOr("MEOAVE_LLM_BASE_URL", "https://api.mpas.top/v1"), "OpenAI-compatible API base URL")
	model := flag.String("model", envOr("MEOAVE_LLM_MODEL", "gemini-3-flash"), "LLM model")
	text := flag.String("text", "你好，今天感觉怎么样？", "test user message")
	skipTTS := flag.Bool("skip-tts", false, "only test Gemini JSON output")
	flag.Parse()

	key := os.Getenv("MEOAVE_LLM_API_KEY")
	if key == "" {
		fatal("MEOAVE_LLM_API_KEY is not set")
	}
	client, err := llm.New(llm.Config{BaseURL: *base, APIKey: key, Model: *model, Timeout: 60 * time.Second})
	if err != nil {
		fatal(err.Error())
	}
	started := time.Now()
	result, err := client.Complete(context.Background(), systemPrompt(), *text)
	if err != nil {
		fatal(err.Error())
	}
	llmMS := time.Since(started).Milliseconds()

	var parsed reply
	if err := json.Unmarshal([]byte(stripFence(result.Content)), &parsed); err != nil {
		fatal(fmt.Sprintf("Gemini did not return the required JSON contract: %v; content=%q", err, result.Content))
	}
	if len(parsed.Sentences) == 0 {
		fatal("Gemini returned an empty sentence list")
	}
	report := map[string]any{
		"ok": true, "base_url": *base, "model": result.Model, "request_model": *model,
		"llm_ms": llmMS, "sentence_count": len(parsed.Sentences), "response_id": result.ID,
		"sentences": make([]any, 0, len(parsed.Sentences)),
	}
	items := report["sentences"].([]any)
	for i, sentence := range parsed.Sentences {
		request := protocol.SentenceRequest{Generation: 1, SentenceID: uint32(i + 1), Text: sentence.Text, Expressions: sentence.Expressions}
		if err := request.ValidateStructural(); err != nil {
			fatal(fmt.Sprintf("sentence %d contract rejected: %v", i+1, err))
		}
		item := map[string]any{"sentence_id": i + 1, "text": sentence.Text, "cue_count": len(sentence.Expressions)}
		if !*skipTTS {
			ttsClient, err := tts.NewEdge(tts.Config{Voice: "zh-CN-XiaoxiaoNeural"})
			if err != nil {
				fatal(err.Error())
			}
			ttsResult, err := ttsClient.Synthesize(context.Background(), sentence.Text)
			if err != nil {
				fatal(fmt.Sprintf("sentence %d Edge-TTS failed: %v", i+1, err))
			}
			decoded, err := audio.DecodeMP3(ttsResult.MP3)
			if err != nil {
				fatal(err.Error())
			}
			samples, err := audio.ResampleMonoS16(decoded.Samples, decoded.SampleRate, protocol.SampleRate)
			if err != nil {
				fatal(err.Error())
			}
			if err := request.ValidateForFrames(len(samples)); err != nil {
				fatal(fmt.Sprintf("sentence %d timing rejected after TTS: %v", i+1, err))
			}
			pcm := audio.EncodeS16LE(samples)
			hash := sha256.Sum256(pcm)
			item["frames"] = len(samples)
			item["pcm_sha256"] = hex.EncodeToString(hash[:])
			item["tts_ms"] = ttsResult.FinishedAt.Sub(ttsResult.StartedAt).Milliseconds()
		}
		items = append(items, item)
	}
	report["sentences"] = items
	encoded, _ := json.MarshalIndent(report, "", "  ")
	fmt.Println(string(encoded))
}

func systemPrompt() string {
	return strings.TrimSpace(`你是 Meoave 的对话编排器。只返回 JSON，不要 Markdown、解释或代码围栏。
格式必须是：{"sentences":[{"text":"...","expressions":[{"at_ms":0,"expression":"CURIOUS","intensity":0.5,"transition_ms":160}]}]}
要求：每个句子都必须有非空 expressions；每条链首 cue 的 at_ms 必须为 0；expression 只能是 CALM、HAPPY、CURIOUS、SLEEPY、SURPRISED、WINK、SAD、EXCITED、SHY、LOVE、PROUD、CONFUSED、WORRIED、ANGRY、FOCUSED、RELIEVED；intensity 为 0 到 1；transition_ms 为 0 到 500；最多返回两句，text 使用中文。`)
}

func stripFence(content string) string {
	content = strings.TrimSpace(content)
	if strings.HasPrefix(content, "```") {
		content = strings.TrimPrefix(content, "```")
		if strings.HasPrefix(content, "json") {
			content = strings.TrimPrefix(content, "json")
		}
		content = strings.TrimSpace(strings.TrimSuffix(content, "```"))
	}
	return content
}

func envOr(key, fallback string) string {
	if value := os.Getenv(key); value != "" {
		return value
	}
	return fallback
}

func fatal(message string) { fmt.Fprintln(os.Stderr, message); os.Exit(1) }
