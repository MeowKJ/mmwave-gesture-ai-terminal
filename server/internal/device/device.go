package device

import (
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/meoave/meoave/server/internal/protocol"
	"github.com/meoave/meoave/server/internal/voicewire"
)

type SentenceMeta struct {
	V           int            `json:"v"`
	Generation  uint32         `json:"generation"`
	SentenceID  uint32         `json:"sentence_id"`
	SampleRate  int            `json:"sample_rate"`
	Channels    int            `json:"channels"`
	Format      string         `json:"format"`
	Frames      int            `json:"frames"`
	Text        string         `json:"text"`
	Expressions []protocol.Cue `json:"expressions"`
}

type Receipt struct {
	Port           string `json:"port"`
	Generation     uint32 `json:"generation"`
	SentenceID     uint32 `json:"sentence_id"`
	Frames         int    `json:"frames"`
	PlayedFrames   int    `json:"played_frames"`
	ReportedFrames int    `json:"reported_frames"`
	Error          int    `json:"error"`
	Done           string `json:"done"`
}

type Device struct {
	portName string
	baud     int
	mu       sync.Mutex
	serial   *serialPort
}

func Open(port string, baud int) (*Device, error) {
	if baud <= 0 {
		baud = 460800
	}
	serial, err := OpenSerial(port, baud)
	if err != nil {
		return nil, err
	}
	return &Device{portName: port, baud: baud, serial: serial}, nil
}

func (d *Device) Close() error {
	if d == nil {
		return nil
	}
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.serial == nil {
		return nil
	}
	err := d.serial.Close()
	d.serial = nil
	return err
}

func (d *Device) Play(ctx context.Context, response protocol.SentenceResponse) (Receipt, error) {
	if d == nil || d.serial == nil {
		return Receipt{}, fmt.Errorf("device transport is not configured")
	}
	pcm, err := base64.StdEncoding.DecodeString(response.AudioBase64)
	if err != nil {
		return Receipt{}, fmt.Errorf("decode audio: %w", err)
	}
	if len(pcm) != response.Frames*2 {
		return Receipt{}, fmt.Errorf("audio length %d does not match %d frames", len(pcm), response.Frames)
	}
	hash := sha256.Sum256(pcm)
	if hex.EncodeToString(hash[:]) != strings.ToLower(response.SHA256) {
		return Receipt{}, fmt.Errorf("audio sha256 mismatch")
	}
	meta, err := json.Marshal(SentenceMeta{
		V: response.V, Generation: response.Generation, SentenceID: response.SentenceID,
		SampleRate: response.SampleRate, Channels: response.Channels, Format: response.Format,
		Frames: response.Frames, Text: response.Text, Expressions: response.Expressions,
	})
	if err != nil {
		return Receipt{}, err
	}

	d.mu.Lock()
	defer d.mu.Unlock()
	// The firmware stores the token in a 16-byte buffer including the NUL.
	// Keep the complete token at <=15 printable bytes so replies can be matched
	// exactly instead of silently matching a truncated firmware token.
	token := fmt.Sprintf("m%014x", uint64(time.Now().UnixNano())&0x0fffffffffffffff)
	if err := d.writeLine(ctx, fmt.Sprintf("VOICE OPEN %s\n", token)); err != nil {
		return Receipt{}, fmt.Errorf("send VOICE OPEN: %w", err)
	}
	openCtx, cancel := context.WithTimeout(ctx, 3*time.Second)
	defer cancel()
	openReply, err := d.waitLineText(openCtx, func(line string) bool {
		return strings.Contains(line, "token="+token) &&
			(strings.Contains(line, "MEO:VOICE_OPEN") || strings.Contains(line, "MEO:VOICE_NACK"))
	})
	if err != nil {
		return Receipt{}, fmt.Errorf("wait for MEO:VOICE_OPEN token=%s: %w", token, err)
	}
	if !strings.Contains(openReply, "token="+token) || !strings.Contains(openReply, "accepted=1") {
		return Receipt{}, fmt.Errorf("unexpected VOICE OPEN reply for token=%s: %s", token, openReply)
	}

	seq := uint32(0)
	metaChunks := chunk(meta, voicewire.MaxPayload)
	for i, part := range metaChunks {
		flags := byte(0)
		if i == 0 {
			flags |= voicewire.First
		}
		if i == len(metaChunks)-1 {
			flags |= voicewire.Last
		}
		frame, err := voicewire.Frame(response.Generation, response.SentenceID, seq, voicewire.Meta, flags, part)
		if err != nil {
			return Receipt{}, err
		}
		if err := d.serial.WriteContext(ctx, frame); err != nil {
			return Receipt{}, err
		}
		seq++
	}
	// The P4 enables the external power amplifier before it accepts the first
	// PCM block.  Leave a small lead-in so the bounded device queue does not
	// fill during that 150 ms startup window; otherwise a perfectly paced host
	// can be rejected with PCM_BACKPRESSURE before playback begins.
	select {
	case <-ctx.Done():
		return Receipt{}, ctx.Err()
	case <-time.After(500 * time.Millisecond):
	}
	// Native USB CDC writes can be acknowledged by Windows before the bytes
	// have reached the P4.  Sleep-based pacing therefore still allows a large
	// burst into the device-side queue and produces PCM_BACKPRESSURE.  The
	// firmware acknowledges every accepted PCM frame; use that acknowledgement
	// as the flow-control boundary instead of guessing from wall-clock time.
	for offset := 0; offset < len(pcm); {
		end := offset + voicewire.MaxPayload
		if end > len(pcm) {
			end = len(pcm)
		}
		if (end-offset)%2 != 0 {
			end--
		}
		frame, err := voicewire.Frame(response.Generation, response.SentenceID, seq, voicewire.PCM, 0, pcm[offset:end])
		if err != nil {
			return Receipt{}, err
		}
		if err := d.serial.WriteContext(ctx, frame); err != nil {
			return Receipt{}, err
		}
		ackCtx, cancelAck := context.WithTimeout(ctx, 2*time.Second)
		ack, ackErr := d.waitLineText(ackCtx, func(line string) bool {
			return (strings.Contains(line, "MEO:VOICE_NACK") && strings.Contains(line, "token="+token)) ||
				(strings.Contains(line, "MEO:VOICE_PCM_ACCEPTED") &&
					strings.Contains(line, "token="+token) &&
					strings.Contains(line, fmt.Sprintf("seq=%d", seq)))
		})
		cancelAck()
		if ackErr != nil {
			return Receipt{}, fmt.Errorf("wait for PCM acceptance seq=%d: %w", seq, ackErr)
		}
		if strings.Contains(ack, "MEO:VOICE_NACK") {
			return Receipt{}, fmt.Errorf("device rejected PCM seq=%d: %s", seq, ack)
		}
		seq++
		offset = end
	}
	endFrame, err := voicewire.Frame(response.Generation, response.SentenceID, seq, voicewire.End, voicewire.Last, nil)
	if err != nil {
		return Receipt{}, err
	}
	if err := d.serial.WriteContext(ctx, endFrame); err != nil {
		return Receipt{}, err
	}

	doneCtx, cancelDone := context.WithTimeout(ctx, 45*time.Second)
	defer cancelDone()
	doneReply, err := d.waitLineText(doneCtx, func(line string) bool {
		return (strings.Contains(line, "MEO:VOICE_NACK") && strings.Contains(line, "token="+token)) ||
			(strings.Contains(line, "MEO:VOICE_SENTENCE_DONE") &&
				strings.Contains(line, fmt.Sprintf("generation=%d", response.Generation)) &&
				strings.Contains(line, fmt.Sprintf("sentence=%d", response.SentenceID)))
	})
	if err != nil {
		return Receipt{}, fmt.Errorf("wait for MEO:VOICE_SENTENCE_DONE: %w", err)
	}
	if !strings.Contains(doneReply, "error=0") {
		return Receipt{}, fmt.Errorf("device completed voice with error: %s", doneReply)
	}
	played, ok := lineUint(doneReply, "played")
	if !ok {
		return Receipt{}, fmt.Errorf("device completion has no played frame count: %s", doneReply)
	}
	reported, ok := lineUint(doneReply, "frames")
	if !ok || int(reported) != response.Frames || int(played) != response.Frames {
		return Receipt{}, fmt.Errorf("device completion frame mismatch: played=%d reported=%d expected=%d", played, reported, response.Frames)
	}
	return Receipt{Port: d.portName, Generation: response.Generation, SentenceID: response.SentenceID,
		Frames: response.Frames, PlayedFrames: int(played), ReportedFrames: int(reported), Error: 0, Done: "played"}, nil
}

func (d *Device) writeLine(ctx context.Context, line string) error {
	return d.serial.WriteContext(ctx, []byte(line))
}

func (d *Device) waitLineText(ctx context.Context, predicate func(string) bool) (string, error) {
	var line []byte
	buf := make([]byte, 256)
	for {
		n, err := d.serial.ReadContext(ctx, buf)
		if err != nil {
			return "", err
		}
		if n == 0 {
			continue
		}
		line = append(line, buf[:n]...)
		for {
			idx := -1
			for i, b := range line {
				if b == '\n' {
					idx = i
					break
				}
			}
			if idx < 0 {
				if len(line) > 4096 {
					return "", fmt.Errorf("device line exceeds 4096 bytes")
				}
				break
			}
			text := strings.TrimSpace(string(line[:idx]))
			line = line[idx+1:]
			if predicate(text) {
				return text, nil
			}
		}
	}
}

func lineUint(line, key string) (uint64, bool) {
	for _, field := range strings.Fields(line) {
		prefix := key + "="
		if strings.HasPrefix(field, prefix) {
			value, err := strconv.ParseUint(strings.TrimPrefix(field, prefix), 10, 32)
			return value, err == nil
		}
	}
	return 0, false
}

func chunk(data []byte, size int) [][]byte {
	if len(data) == 0 {
		return [][]byte{nil}
	}
	var out [][]byte
	for len(data) > 0 {
		n := size
		if n > len(data) {
			n = len(data)
		}
		out = append(out, data[:n])
		data = data[n:]
	}
	return out
}
