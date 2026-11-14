//go:build windows

package device

import (
	"context"
	"fmt"
	"strings"
	"syscall"
	"time"
	"unsafe"
)

const (
	genericRead       = 0x80000000
	genericWrite      = 0x40000000
	openExisting      = 3
	fileAttributeNorm = 0x80
	maxPath           = 32767
)

type dcb struct {
	DCBlength  uint32
	BaudRate   uint32
	Flags      uint32
	WReserved  uint16
	XonLim     uint16
	XoffLim    uint16
	ByteSize   uint8
	Parity     uint8
	StopBits   uint8
	XonChar    int8
	XoffChar   int8
	ErrorChar  int8
	EofChar    int8
	EvtChar    int8
	WReserved1 uint16
}

type commTimeouts struct {
	ReadIntervalTimeout         uint32
	ReadTotalTimeoutMultiplier  uint32
	ReadTotalTimeoutConstant    uint32
	WriteTotalTimeoutMultiplier uint32
	WriteTotalTimeoutConstant   uint32
}

type serialPort struct {
	h syscall.Handle
}

var (
	kernel32           = syscall.NewLazyDLL("kernel32.dll")
	createFileW        = kernel32.NewProc("CreateFileW")
	closeHandle        = kernel32.NewProc("CloseHandle")
	getCommState       = kernel32.NewProc("GetCommState")
	setCommState       = kernel32.NewProc("SetCommState")
	setCommTimeouts    = kernel32.NewProc("SetCommTimeouts")
	setupComm          = kernel32.NewProc("SetupComm")
	purgeComm          = kernel32.NewProc("PurgeComm")
	escapeCommFunction = kernel32.NewProc("EscapeCommFunction")
	readFile           = kernel32.NewProc("ReadFile")
	writeFile          = kernel32.NewProc("WriteFile")
)

const (
	setRTS = 3
	setDTR = 5
)

func OpenSerial(port string, baud int) (*serialPort, error) {
	port = strings.TrimSpace(port)
	if port == "" {
		return nil, fmt.Errorf("serial port is empty")
	}
	path := port
	if !strings.HasPrefix(strings.ToUpper(path), `\\.\`) {
		path = `\\.\` + path
	}
	name, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return nil, err
	}
	r, _, callErr := createFileW.Call(uintptr(unsafe.Pointer(name)), genericRead|genericWrite, 0, 0,
		openExisting, fileAttributeNorm, 0)
	if r == uintptr(syscall.InvalidHandle) {
		return nil, fmt.Errorf("open %s: %w", port, callErr)
	}
	s := &serialPort{h: syscall.Handle(r)}
	if err := s.configure(baud); err != nil {
		s.Close()
		return nil, err
	}
	return s, nil
}

func (s *serialPort) configure(baud int) error {
	var state dcb
	state.DCBlength = uint32(unsafe.Sizeof(state))
	if ok, _, err := getCommState.Call(uintptr(s.h), uintptr(unsafe.Pointer(&state))); ok == 0 {
		return fmt.Errorf("GetCommState: %w", err)
	}
	state.BaudRate = uint32(baud)
	state.ByteSize, state.Parity, state.StopBits = 8, 0, 0
	// Native USB CDC on Windows follows the line-control state.  Explicitly
	// request binary 8N1 with DTR/RTS asserted, matching pyserial and the
	// firmware's known-good diagnostic tools.  Leaving the inherited DCB flags
	// untouched can make usbser.sys accept the handle but reject the first
	// WriteFile with ERROR_GEN_FAILURE (31).
	state.Flags = 1 | 0x10 | 0x1000 // fBinary + DTR/RTS_CONTROL_ENABLE
	if ok, _, err := setCommState.Call(uintptr(s.h), uintptr(unsafe.Pointer(&state))); ok == 0 {
		return fmt.Errorf("SetCommState: %w", err)
	}
	if ok, _, err := escapeCommFunction.Call(uintptr(s.h), setDTR); ok == 0 {
		return fmt.Errorf("EscapeCommFunction(SETDTR): %w", err)
	}
	if ok, _, err := escapeCommFunction.Call(uintptr(s.h), setRTS); ok == 0 {
		return fmt.Errorf("EscapeCommFunction(SETRTS): %w", err)
	}
	if ok, _, err := setupComm.Call(uintptr(s.h), 4096, 4096); ok == 0 {
		return fmt.Errorf("SetupComm: %w", err)
	}
	t := commTimeouts{ReadIntervalTimeout: 20, ReadTotalTimeoutConstant: 20}
	if ok, _, err := setCommTimeouts.Call(uintptr(s.h), uintptr(unsafe.Pointer(&t))); ok == 0 {
		return fmt.Errorf("SetCommTimeouts: %w", err)
	}
	_, _, _ = purgeComm.Call(uintptr(s.h), 0x0008|0x0004) // PURGE_RXCLEAR | PURGE_TXCLEAR
	return nil
}

func (s *serialPort) Close() error {
	if s == nil || s.h == 0 {
		return nil
	}
	ok, _, err := closeHandle.Call(uintptr(s.h))
	s.h = 0
	if ok == 0 {
		return err
	}
	return nil
}

func (s *serialPort) WriteContext(ctx context.Context, data []byte) error {
	for len(data) > 0 {
		select {
		case <-ctx.Done():
			return ctx.Err()
		default:
		}
		var written uint32
		ok, _, err := writeFile.Call(uintptr(s.h), uintptr(unsafe.Pointer(&data[0])), uintptr(len(data)), uintptr(unsafe.Pointer(&written)), 0)
		if ok == 0 {
			return fmt.Errorf("WriteFile: %w", err)
		}
		if written == 0 {
			time.Sleep(time.Millisecond)
			continue
		}
		data = data[written:]
	}
	return nil
}

func (s *serialPort) ReadContext(ctx context.Context, buf []byte) (int, error) {
	if len(buf) == 0 {
		return 0, nil
	}
	for {
		select {
		case <-ctx.Done():
			return 0, ctx.Err()
		default:
		}
		var read uint32
		ok, _, err := readFile.Call(uintptr(s.h), uintptr(unsafe.Pointer(&buf[0])), uintptr(len(buf)), uintptr(unsafe.Pointer(&read)), 0)
		if ok == 0 {
			return 0, fmt.Errorf("ReadFile: %w", err)
		}
		if read == 0 {
			return 0, nil
		}
		return int(read), nil
	}
}
