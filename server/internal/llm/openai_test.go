package llm

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestCompactBoundsProviderError(t *testing.T) {
	if got := compact([]byte("a  b\n c")); got != "a b c" {
		t.Fatalf("got %q", got)
	}
	if got := compact(make([]byte, 600)); len(got) != 515 {
		t.Fatalf("bounded error length is %d", len(got))
	}
}

func TestOptionalJSONAndNonThinkingSettingsReachConfiguredEndpoint(t *testing.T) {
	upstream := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/chat/completions" || r.Header.Get("Authorization") != "Bearer offline-test-key" {
			t.Error("wrong endpoint or authorization")
		}
		var req chatRequest
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			t.Error(err)
		}
		if req.Model != "configured-model" || req.ReasoningEffort != "none" || req.ResponseFormat == nil || req.ResponseFormat.Type != "json_object" {
			t.Errorf("wrong request options: %+v", req)
		}
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"id":"offline","model":"configured-model","choices":[{"message":{"role":"assistant","content":"{}"}}]}`))
	}))
	defer upstream.Close()
	c, err := New(Config{BaseURL: upstream.URL, APIKey: "offline-test-key", Model: "configured-model", ReasoningEffort: "none", JSONOutput: true})
	if err != nil {
		t.Fatal(err)
	}
	if _, err = c.Complete(context.Background(), "Return JSON", "hello"); err != nil {
		t.Fatal(err)
	}
}
