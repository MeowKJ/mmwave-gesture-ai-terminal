//go:build !windows

package device

import (
	"context"
	"fmt"
)

type serialPort struct{}

func OpenSerial(string, int) (*serialPort, error) {
	return nil, fmt.Errorf("native CDC transport is currently implemented for Windows only")
}
func (*serialPort) Close() error { return nil }
func (*serialPort) WriteContext(context.Context, []byte) error {
	return fmt.Errorf("native CDC transport is currently implemented for Windows only")
}
func (*serialPort) ReadContext(context.Context, []byte) (int, error) {
	return 0, fmt.Errorf("native CDC transport is currently implemented for Windows only")
}
