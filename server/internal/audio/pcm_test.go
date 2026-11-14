package audio

import "testing"

func TestResample24To48KeepsExpectedFrameCount(t *testing.T) {
	in := []int16{0, 1000, -1000, 2000}
	out, err := ResampleMonoS16(in, 24000, 48000)
	if err != nil {
		t.Fatal(err)
	}
	if len(out) != 8 {
		t.Fatalf("got %d frames, want 8", len(out))
	}
	if out[0] != 0 || out[len(out)-1] != 2000 {
		t.Fatalf("unexpected endpoints: %v", out)
	}
}

func TestResampleRejectsInvalidInput(t *testing.T) {
	if _, err := ResampleMonoS16(nil, 24000, 48000); err == nil {
		t.Fatal("expected empty PCM to fail")
	}
	if _, err := ResampleMonoS16([]int16{1}, 0, 48000); err == nil {
		t.Fatal("expected invalid rate to fail")
	}
}
