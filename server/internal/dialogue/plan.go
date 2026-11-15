// Package dialogue validates model output before it can reach audio or a device.
package dialogue

import (
	"encoding/json"
	"fmt"
	"io"
	"strings"

	"github.com/meoave/meoave/server/internal/protocol"
)

const SystemPrompt = `你是桌面陪伴猫猫 Meoave。用温柔自然的中文回应用户，不自称开发板，不编造感知或联网能力。
只返回 JSON，不要 Markdown：{"sentences":[{"text":"你好呀。","expressions":[{"at_ms":0,"expression":"CURIOUS","intensity":0.55,"transition_ms":160}]}]}
回复一到两句，每项只放一句，每句尽量不超过30个汉字。每句必须附带非空 expressions 表情链。
首个 at_ms 必须为0；后续时间严格递增，过渡不能重叠。表情只能是 CALM、HAPPY、CURIOUS、SLEEPY、SURPRISED、WINK、SAD、EXCITED、SHY、LOVE、PROUD、CONFUSED、WORRIED、ANGRY、FOCUSED、RELIEVED。
intensity 为0到1，transition_ms 为0到200。若增加第二个表情，放在600ms，不要把表情安排在句末音频之外。`

type Sentence struct {
	Text        string         `json:"text"`
	Expressions []protocol.Cue `json:"expressions"`
}

type Plan struct {
	Sentences []Sentence `json:"sentences"`
}

func Parse(content string) (Plan, error) {
	content = strings.TrimSpace(content)
	if strings.HasPrefix(content, "```json\n") && strings.HasSuffix(content, "```") {
		content = strings.TrimSpace(strings.TrimSuffix(strings.TrimPrefix(content, "```json\n"), "```"))
	}
	var plan Plan
	d := json.NewDecoder(strings.NewReader(content))
	d.DisallowUnknownFields()
	if err := d.Decode(&plan); err != nil {
		return Plan{}, fmt.Errorf("invalid dialogue plan: %w", err)
	}
	var extra any
	if err := d.Decode(&extra); err != io.EOF {
		return Plan{}, fmt.Errorf("dialogue plan contains trailing data")
	}
	if len(plan.Sentences) < 1 || len(plan.Sentences) > 2 {
		return Plan{}, fmt.Errorf("dialogue plan must contain one or two sentences")
	}
	for i, sentence := range plan.Sentences {
		req := protocol.SentenceRequest{Generation: 1, SentenceID: uint32(i + 1), Text: sentence.Text, Expressions: sentence.Expressions}
		if err := req.ValidateStructural(); err != nil {
			return Plan{}, fmt.Errorf("dialogue sentence %d: %w", i+1, err)
		}
	}
	return plan, nil
}
