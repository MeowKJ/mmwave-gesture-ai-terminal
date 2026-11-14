package audio

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math"

	"github.com/hajimehoshi/go-mp3"
)

type PCM struct {
	SampleRate int
	Channels   int
	Samples    []int16
}

func DecodeMP3(data []byte) (PCM, error) {
	if len(data) == 0 {
		return PCM{}, errors.New("MP3 data is empty")
	}
	decoder, err := mp3.NewDecoder(bytes.NewReader(data))
	if err != nil {
		return PCM{}, fmt.Errorf("decode MP3: %w", err)
	}
	decoded, err := io.ReadAll(decoder)
	if err != nil {
		return PCM{}, fmt.Errorf("read decoded MP3: %w", err)
	}
	if len(decoded) == 0 || len(decoded)%4 != 0 {
		return PCM{}, fmt.Errorf("decoded MP3 is not complete stereo S16: %d bytes", len(decoded))
	}
	mono := make([]int16, len(decoded)/4)
	for i := range mono {
		left := int16(binary.LittleEndian.Uint16(decoded[i*4:]))
		right := int16(binary.LittleEndian.Uint16(decoded[i*4+2:]))
		mono[i] = int16((int32(left) + int32(right)) / 2)
	}
	return PCM{SampleRate: decoder.SampleRate(), Channels: 1, Samples: mono}, nil
}

func ResampleMonoS16(src []int16, inputRate, outputRate int) ([]int16, error) {
	if inputRate <= 0 || outputRate <= 0 {
		return nil, errors.New("sample rates must be positive")
	}
	if len(src) == 0 {
		return nil, errors.New("source PCM is empty")
	}
	if inputRate == outputRate {
		return append([]int16(nil), src...), nil
	}
	outLen := int(math.Round(float64(len(src)) * float64(outputRate) / float64(inputRate)))
	if outLen < 1 {
		outLen = 1
	}
	out := make([]int16, outLen)
	for i := range out {
		position := float64(i) * float64(inputRate) / float64(outputRate)
		left := int(position)
		if left >= len(src)-1 {
			out[i] = src[len(src)-1]
			continue
		}
		fraction := position - float64(left)
		value := float64(src[left])*(1-fraction) + float64(src[left+1])*fraction
		if value > math.MaxInt16 {
			value = math.MaxInt16
		} else if value < math.MinInt16 {
			value = math.MinInt16
		}
		out[i] = int16(math.Round(value))
	}
	return out, nil
}

func EncodeS16LE(samples []int16) []byte {
	data := make([]byte, len(samples)*2)
	for i, sample := range samples {
		binary.LittleEndian.PutUint16(data[i*2:], uint16(sample))
	}
	return data
}
