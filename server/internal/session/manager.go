package session

import (
	"context"
	"fmt"
	"sync"
)

type Manager struct {
	mu      sync.Mutex
	current uint32
	active  map[uint32]context.CancelFunc
}

func NewManager() *Manager { return &Manager{active: make(map[uint32]context.CancelFunc)} }

func (m *Manager) Start(parent context.Context, generation uint32) (context.Context, error) {
	if generation == 0 {
		return nil, fmt.Errorf("generation must be non-zero")
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	if generation <= m.current {
		return nil, fmt.Errorf("generation %d is not newer than current generation %d", generation, m.current)
	}
	for _, cancel := range m.active {
		cancel()
	}
	ctx, cancel := context.WithCancel(parent)
	m.current = generation
	m.active = map[uint32]context.CancelFunc{generation: cancel}
	return ctx, nil
}

func (m *Manager) Finish(generation uint32) {
	m.mu.Lock()
	defer m.mu.Unlock()
	delete(m.active, generation)
}

func (m *Manager) Cancel(generation uint32) bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	if generation == 0 {
		generation = m.current
	}
	cancel, ok := m.active[generation]
	if ok {
		cancel()
		delete(m.active, generation)
	}
	return ok
}

func (m *Manager) Current() uint32 {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.current
}
