package llm

import "testing"

func TestCompactBoundsProviderError(t *testing.T) {
	if got := compact([]byte("a  b\n c")); got != "a b c" {
		t.Fatalf("got %q", got)
	}
	if got := compact(make([]byte, 600)); len(got) != 515 {
		t.Fatalf("bounded error length is %d", len(got))
	}
}
