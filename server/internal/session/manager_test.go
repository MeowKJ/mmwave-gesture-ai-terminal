package session

import (
	"context"
	"testing"
)

func TestNewGenerationCancelsPreviousAndRejectsReuse(t *testing.T) {
	m := NewManager()
	first, err := m.Start(context.Background(), 1)
	if err != nil {
		t.Fatal(err)
	}
	second, err := m.Start(context.Background(), 2)
	if err != nil {
		t.Fatal(err)
	}
	select {
	case <-first.Done():
	default:
		t.Fatal("starting a newer generation did not cancel the older one")
	}
	if _, err := m.Start(context.Background(), 2); err == nil {
		t.Fatal("expected generation reuse to be rejected")
	}
	if second.Err() != nil {
		t.Fatalf("new generation unexpectedly canceled: %v", second.Err())
	}
}

func TestCancelCurrentGeneration(t *testing.T) {
	m := NewManager()
	ctx, err := m.Start(context.Background(), 7)
	if err != nil {
		t.Fatal(err)
	}
	if !m.Cancel(0) {
		t.Fatal("expected current generation to be canceled")
	}
	select {
	case <-ctx.Done():
	default:
		t.Fatal("cancel did not close context")
	}
}
