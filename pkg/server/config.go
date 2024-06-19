package server

import (
	"time"

	"github.com/ungive/loon/pkg/pb"
)

type Config struct {
	Constraints *pb.Constraints    `json:"constraints"`
	Intervals   *ProtocolIntervals `json:"intervals"`
}

type ProtocolIntervals struct {
	// The duration after which a message should have been written.
	// If the message was not fully written within this time frame,
	// the connection is closed.
	WriteWait time.Duration `json:"write_wait"`

	// The duration after which a pong message should have arrived.
	// If no pong is received within this time frame, the connection is closed.
	PongWait time.Duration `json:"pong_wait"`

	// The interval at which websocket ping messages are sent.
	PingInterval time.Duration `json:"ping_interval"`

	// The duration after which an expected message should have arrived.
	// If no message is received within this time frame,
	// the connection is closed.
	TimeoutDuration time.Duration `json:"timeout_duration"`

	// The interval at which message timeouts are checked.
	TimeoutInterval time.Duration `json:"timeout_interval"`
}
