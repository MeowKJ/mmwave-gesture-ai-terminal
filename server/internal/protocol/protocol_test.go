package protocol

import "testing"

func validRequest() SentenceRequest {
	return SentenceRequest{
		Generation: 3, SentenceID: 1, Text: "你好。",
		Expressions: []Cue{{AtMS: 0, Expression: "CURIOUS", Intensity: .5, TransitionMS: 100}, {AtMS: 500, Expression: "RELIEVED", Intensity: .7, TransitionMS: 100}},
	}
}

func TestRejectsEmptyText(t *testing.T) {
	r := validRequest()
	r.Text = " \n"
	if err := r.ValidateStructural(); err == nil {
		t.Fatal("expected empty text to be rejected")
	}
}

func TestRejectsMissingExpressionChain(t *testing.T) {
	r := validRequest()
	r.Expressions = nil
	if err := r.ValidateStructural(); err == nil {
		t.Fatal("expected missing chain to be rejected")
	}
}

func TestRejectsIntensityOutOfRange(t *testing.T) {
	r := validRequest()
	r.Expressions[0].Intensity = 1.1
	if err := r.ValidateStructural(); err == nil {
		t.Fatal("expected intensity to be rejected")
	}
}

func TestRejectsCueOrder(t *testing.T) {
	r := validRequest()
	r.Expressions[1].AtMS = 0
	if err := r.ValidateStructural(); err == nil {
		t.Fatal("expected cue order to be rejected")
	}
}

func TestRejectsTransitionPastNextCue(t *testing.T) {
	r := validRequest()
	r.Expressions[0].TransitionMS = 1000
	if err := r.ValidateForFrames(48000); err == nil {
		t.Fatal("expected overlap to be rejected")
	}
}

func TestKeepsGenerationAndSentenceID(t *testing.T) {
	r := validRequest()
	response := r.Response("AQI=", "hash", 48000, Timing{})
	if response.Generation != 3 || response.SentenceID != 1 {
		t.Fatalf("identity changed: %#v", response)
	}
}

func TestDecodeStrictRejectsUnknownAndTrailingFields(t *testing.T) {
	json := `{"generation":1,"sentence_id":1,"text":"x","expressions":[{"at_ms":0,"expression":"CALM","intensity":0.5,"transition_ms":0}],"extra":true}`
	if err := DecodeStrict([]byte(json), &SentenceRequest{}); err == nil {
		t.Fatal("expected unknown field to be rejected")
	}
	json = `{"generation":1,"sentence_id":1,"text":"x","expressions":[{"at_ms":0,"expression":"CALM","intensity":0.5,"transition_ms":0}]} {}`
	if err := DecodeStrict([]byte(json), &SentenceRequest{}); err == nil {
		t.Fatal("expected trailing JSON to be rejected")
	}
}
