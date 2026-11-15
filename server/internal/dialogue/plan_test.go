package dialogue

import (
	"strings"
	"testing"
)

func TestPlanRequiresAChainForEverySentence(t *testing.T) {
	valid := `{"sentences":[{"text":"我在这里。","expressions":[{"at_ms":0,"expression":"HAPPY","intensity":0.6,"transition_ms":160}]}]}`
	for _, content := range []string{valid, "```json\n" + valid + "\n```"} {
		p, err := Parse(content)
		if err != nil || len(p.Sentences) != 1 {
			t.Fatalf("valid plan rejected: %v", err)
		}
	}
	for _, bad := range []string{
		`{"sentences":[]}`,
		`{"sentences":[{"text":"你好。","expressions":[]}]}`,
		strings.Replace(valid, "HAPPY", "GPIO_ON", 1),
		strings.Replace(valid, `"intensity":0.6`, `"intensity":2`, 1),
		strings.Replace(valid, `"at_ms":0`, `"at_ms":100`, 1),
		strings.Replace(valid, `"text":`, `"command":"RESET","text":`, 1),
		valid + " {}",
		`{"sentences":[{"text":"可以。","expressions":[{"at_ms":0,"expression":"CALM","intensity":0.5,"transition_ms":0}]},{"text":"后句缺表情。"}]}`,
	} {
		if _, err := Parse(bad); err == nil {
			t.Fatalf("invalid plan accepted: %s", bad)
		}
	}
}
