package voicewire

import (
	"encoding/binary"
	"fmt"
)

const (
	Version    = 1
	HeaderSize = 24
	MaxPayload = 960
)

const (
	Meta   byte = 1
	PCM    byte = 2
	End    byte = 3
	Cancel byte = 4
)

const (
	First byte = 1 << 0
	Last  byte = 1 << 1
)

// Frame is the native-USB CDC binary frame.  CDC itself is a byte stream, so
// every frame carries identity, sequence, payload length and CRC16.
func Frame(generation, sentenceID, sequence uint32, kind, flags byte, payload []byte) ([]byte, error) {
	if len(payload) > MaxPayload {
		return nil, fmt.Errorf("voice payload is %d bytes, maximum is %d", len(payload), MaxPayload)
	}
	b := make([]byte, HeaderSize+len(payload))
	b[0], b[1], b[2], b[3] = 'M', 'V', 'O', '1'
	b[4], b[5], b[6] = Version, kind, flags
	binary.LittleEndian.PutUint32(b[8:12], generation)
	binary.LittleEndian.PutUint32(b[12:16], sentenceID)
	binary.LittleEndian.PutUint32(b[16:20], sequence)
	binary.LittleEndian.PutUint16(b[20:22], uint16(len(payload)))
	copy(b[HeaderSize:], payload)
	binary.LittleEndian.PutUint16(b[22:24], crc16(b))
	return b, nil
}

func crc16(data []byte) uint16 {
	var crc uint16 = 0xffff
	for i, value := range data {
		if i == 22 || i == 23 {
			value = 0
		}
		crc ^= uint16(value) << 8
		for bit := 0; bit < 8; bit++ {
			if crc&0x8000 != 0 {
				crc = (crc << 1) ^ 0x1021
			} else {
				crc <<= 1
			}
		}
	}
	return crc
}
