package llm

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"
)

type Config struct {
	BaseURL         string
	APIKey          string
	Model           string
	Timeout         time.Duration
	ReasoningEffort string
	JSONOutput      bool
}

type Client struct {
	baseURL         string
	apiKey          string
	model           string
	reasoningEffort string
	jsonOutput      bool
	http            *http.Client
}

type Message struct {
	Role    string `json:"role"`
	Content string `json:"content"`
}

type chatRequest struct {
	Model           string          `json:"model"`
	Messages        []Message       `json:"messages"`
	Temperature     float64         `json:"temperature,omitempty"`
	MaxTokens       int             `json:"max_tokens,omitempty"`
	Stream          bool            `json:"stream,omitempty"`
	ReasoningEffort string          `json:"reasoning_effort,omitempty"`
	ResponseFormat  *responseFormat `json:"response_format,omitempty"`
}

type responseFormat struct {
	Type string `json:"type"`
}

type chatResponse struct {
	ID      string `json:"id"`
	Model   string `json:"model"`
	Choices []struct {
		Message Message `json:"message"`
	} `json:"choices"`
	Usage json.RawMessage `json:"usage"`
}

type CompletionResult struct {
	ID      string
	Model   string
	Content string
	Usage   json.RawMessage
}

func New(config Config) (*Client, error) {
	base := strings.TrimRight(strings.TrimSpace(config.BaseURL), "/")
	parsed, err := url.Parse(base)
	if err != nil || parsed.Scheme != "http" && parsed.Scheme != "https" || parsed.Host == "" {
		return nil, fmt.Errorf("invalid LLM base URL %q", config.BaseURL)
	}
	if config.APIKey == "" {
		return nil, errors.New("LLM API key is empty")
	}
	if config.Model == "" {
		return nil, errors.New("LLM model is empty")
	}
	if config.Timeout <= 0 {
		config.Timeout = 60 * time.Second
	}
	return &Client{
		baseURL:         base,
		apiKey:          config.APIKey,
		model:           config.Model,
		reasoningEffort: config.ReasoningEffort,
		jsonOutput:      config.JSONOutput,
		http:            &http.Client{Timeout: config.Timeout},
	}, nil
}

func (c *Client) Model() string { return c.model }

func (c *Client) Complete(ctx context.Context, systemPrompt, userPrompt string) (CompletionResult, error) {
	request := chatRequest{
		Model: c.model,
		Messages: []Message{
			{Role: "system", Content: systemPrompt},
			{Role: "user", Content: userPrompt},
		},
		Temperature:     0,
		MaxTokens:       768,
		ReasoningEffort: c.reasoningEffort,
	}
	if c.jsonOutput {
		request.ResponseFormat = &responseFormat{Type: "json_object"}
	}
	body, err := json.Marshal(request)
	if err != nil {
		return CompletionResult{}, err
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, c.baseURL+"/chat/completions", bytes.NewReader(body))
	if err != nil {
		return CompletionResult{}, err
	}
	req.Header.Set("Authorization", "Bearer "+c.apiKey)
	req.Header.Set("Content-Type", "application/json")
	resp, err := c.http.Do(req)
	if err != nil {
		return CompletionResult{}, fmt.Errorf("LLM request: %w", err)
	}
	defer resp.Body.Close()
	responseBody, err := io.ReadAll(io.LimitReader(resp.Body, 2*1024*1024))
	if err != nil {
		return CompletionResult{}, fmt.Errorf("read LLM response: %w", err)
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return CompletionResult{}, fmt.Errorf("LLM HTTP %d: %s", resp.StatusCode, compact(responseBody))
	}
	var decoded chatResponse
	if err := json.Unmarshal(responseBody, &decoded); err != nil {
		return CompletionResult{}, fmt.Errorf("decode LLM response: %w", err)
	}
	if len(decoded.Choices) == 0 || strings.TrimSpace(decoded.Choices[0].Message.Content) == "" {
		return CompletionResult{}, errors.New("LLM response has no text choice")
	}
	return CompletionResult{ID: decoded.ID, Model: decoded.Model, Content: decoded.Choices[0].Message.Content, Usage: decoded.Usage}, nil
}

func compact(data []byte) string {
	text := strings.Join(strings.Fields(string(data)), " ")
	if len(text) > 512 {
		return text[:512] + "..."
	}
	return text
}
