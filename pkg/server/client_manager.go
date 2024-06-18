package server

import (
	"errors"
	"sync/atomic"
)

var (
	ErrClientManagerClosed     = errors.New("the client manager has been closed")
	ErrClientAlreadyRegistered = errors.New("a client with that ID is already registered")
	ErrClientNotRegistered     = errors.New("no client with that ID is registered")
)

type ClientManager interface {
	// Run loop for the client manager.
	Run()
	// Registers a client to accept requests via HTTP requests.
	Register(client Client) error
	// Unregisters the client from accepting HTTP requests.
	// Call this function when the client is in a state where it can't
	// provide content anymore, e.g. because it disconnected
	// or the server closed the connection.
	Unregister(client Client) error
	// Returns the client for a given client ID, if it is registered.
	Get(clientID UUID) (Client, error)
	// Closes the manager run loop, closes all connections
	// and returns once all clients have exited their run loop.
	Close()
}

type getMessage struct {
	clientID UUID
	out      chan Client
	outErr   chan error
}

type registerMessage struct {
	client Client
	outErr chan error
}

type clientManager struct {
	dirty      atomic.Bool
	clients    map[UUID]Client
	get        chan *getMessage
	register   chan *registerMessage
	unregister chan Client
	stop       chan struct{}
	done       chan struct{}
	runDone    chan struct{}
}

func NewClientManager() ClientManager {
	return &clientManager{
		dirty:      atomic.Bool{},
		clients:    make(map[UUID]Client),
		get:        make(chan *getMessage),
		register:   make(chan *registerMessage),
		unregister: make(chan Client),
		stop:       make(chan struct{}),
		done:       make(chan struct{}),
		runDone:    make(chan struct{}),
	}
}

func (m *clientManager) Run() {
	if !m.dirty.CompareAndSwap(false, true) {
		panic("client manager is in a dirty state")
	}
	defer func() {
		for _, client := range m.clients {
			client.Close()
		}
		for _, client := range m.clients {
			<-client.Closed()
		}
		m.clients = nil
		close(m.runDone)
	}()
	for {
		select {
		case <-m.stop:
			close(m.done)
			return
		case msg := <-m.get:
			client, ok := m.clients[msg.clientID]
			if !ok {
				msg.outErr <- ErrClientNotRegistered
				continue
			}
			msg.out <- client
		case msg := <-m.register:
			key := msg.client.ID()
			if _, ok := m.clients[key]; ok {
				msg.outErr <- ErrClientAlreadyRegistered
				continue
			}
			m.clients[key] = msg.client
		case client := <-m.unregister:
			key := client.ID()
			if client, ok := m.clients[key]; ok {
				delete(m.clients, key)
				client.Close()
			}
		}
	}
}

func (m *clientManager) Register(client Client) error {
	errChan := make(chan error)
	select {
	case m.register <- &registerMessage{
		client: client,
		outErr: errChan,
	}:
		return nil
	case err := <-errChan:
		return err
	case <-m.done:
		return ErrClientManagerClosed
	}
}

func (m *clientManager) Unregister(client Client) error {
	select {
	case m.unregister <- client:
		return nil
	case <-m.done:
		return ErrClientManagerClosed
	}
}

func (m *clientManager) Get(clientID UUID) (Client, error) {
	outChan := make(chan Client)
	errChan := make(chan error)
	select {
	case m.get <- &getMessage{
		clientID: clientID,
		out:      outChan,
		outErr:   errChan,
	}:
	case <-m.done:
		return nil, ErrClientManagerClosed
	}
	select {
	case client := <-outChan:
		return client, nil
	case err := <-errChan:
		return nil, err
	case <-m.done:
		return nil, ErrClientManagerClosed
	}
}

func (m *clientManager) Close() {
	m.stop <- struct{}{}
	<-m.runDone
}
