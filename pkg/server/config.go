package server

import (
	"errors"
	"fmt"
	"mime"
	"time"

	"github.com/ungive/loon/pkg/pb"
)

type ProtocolOptions struct {
	BaseUrl     string             `json:"base_url"`
	Constraints *pb.Constraints    `json:"constraints"`
	Intervals   *ProtocolIntervals `json:"intervals"`
}

func (c *ProtocolOptions) Validate() error {
	if len(c.BaseUrl) == 0 {
		return errors.New("base URL cannot be empty")
	}
	if err := ValidateProtocolIntervals(c.Intervals); err != nil {
		return err
	}
	if err := ValidateConstraints(c.Constraints); err != nil {
		return err
	}
	return nil
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

func ValidateProtocolIntervals(intervals *ProtocolIntervals) error {
	if intervals.WriteWait <= 0 {
		return errors.New("write wait must be greater than zero")
	}
	if intervals.PongWait <= 0 {
		return errors.New("pong wait must be greater than zero")
	}
	if intervals.PingInterval <= 0 {
		return errors.New("ping interval must be greater than zero")
	}
	if intervals.TimeoutDuration <= 0 {
		return errors.New("timeout duration must be greater than zero")
	}
	if intervals.TimeoutInterval <= 0 {
		return errors.New("timeout interval must be greater than zero")
	}
	return nil
}

func ValidateConstraints(constraints *pb.Constraints) error {
	if constraints.ChunkSize > constraints.MaxContentSize {
		return errors.New(
			"chunk size cannot be larger than maximum content size")
	}
	if len(constraints.AcceptedContentTypes) == 0 {
		return errors.New("accepted content types cannot be empty")
	}
	for _, contentType := range constraints.AcceptedContentTypes {
		sanitized, params, err := mime.ParseMediaType(contentType)
		if err != nil {
			return fmt.Errorf("invalid accepted content type: %w", err)
		}
		if len(params) > 0 {
			return errors.New("content type may not contain parameters")
		}
		if contentType != sanitized {
			return errors.New(
				"content type must be lowercase and contain no spaces")
		}
	}
	return nil
}
