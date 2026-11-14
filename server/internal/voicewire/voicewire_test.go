package voicewire

import (
	"encoding/binary"
	"testing"
)

func TestFrameLayoutAndCRC(t *testing.T) {
	frame, err := Frame(7, 2, 9, PCM, 0, []byte{1, 2, 3, 4})
	if err != nil {
		t.Fatal(err)
	}
	if len(frame) != HeaderSize+4 || string(frame[:4]) != "MVO1" || frame[4] != Version {
		t.Fatalf("unexpected header: %x", frame[:HeaderSize])
	}
	if got := binary.LittleEndian.Uint32(frame[8:12]); got != 7 {
		t.Fatalf("generation=%d", got)
	}
	if got := binary.LittleEndian.Uint32(frame[12:16]); got != 2 {
		t.Fatalf("sentence=%d", got)
	}
	if got := binary.LittleEndian.Uint32(frame[16:20]); got != 9 {
		t.Fatalf("sequence=%d", got)
	}
	if got := binary.LittleEndian.Uint16(frame[20:22]); got != 4 {
		t.Fatalf("payload=%d", got)
	}
	got := binary.LittleEndian.Uint16(frame[22:24])
	if got != crc16(frame) {
		t.Fatalf("crc mismatch got=%04x expected=%04x", got, crc16(frame))
	}
}

func TestFrameRejectsOversize(t *testing.T) {
	if _, err := Frame(1, 1, 0, PCM, 0, make([]byte, MaxPayload+1)); err == nil {
		t.Fatal("expected oversize payload rejection")
	}
}
