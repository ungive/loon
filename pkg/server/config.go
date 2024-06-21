package server

import (
	"errors"
	"fmt"
	"log/slog"
	"mime"
	"time"

	"github.com/ungive/loon/pkg/pb"
)

// Root object for any server configuration file.
type Options struct {
	Protocol *ProtocolOptions `json:"protocol"`
	Http     *HttpOptions     `json:"http"`
	Log      *LogOptions      `json:"log"`
}

type ProtocolOptions struct {
	BaseUrl     string             `json:"base_url"`
	Constraints *pb.Constraints    `json:"constraints"`
	Intervals   *ProtocolIntervals `json:"intervals"`
}

type HttpOptions struct {
	WriteTimeout time.Duration `json:"write_timeout"`
	ReadTimeout  time.Duration `json:"read_timeout"`
	IdleTimeout  time.Duration `json:"idle_timeout"`
}

type LogOptions struct {
	Level slog.Level `json:"level"`
}

func (c *Options) Validate() error {
	if err := c.Protocol.Validate(); err != nil {
		return err
	}
	if err := c.Http.Validate(); err != nil {
		return err
	}
	return nil
}

func (c *ProtocolOptions) Validate() error {
	if len(c.BaseUrl) == 0 {
		return errors.New("base URL cannot be empty")
	}
	if err := c.Intervals.Validate(); err != nil {
		return err
	}
	if err := ValidateConstraints(c.Constraints); err != nil {
		return err
	}
	return nil
}

func (h *HttpOptions) Validate() error {
	if h.WriteTimeout < 0 {
		return errors.New("http write wait must be greater or equal to zero")
	}
	if h.ReadTimeout < 0 {
		return errors.New("http write wait must be greater or equal to zero")
	}
	if h.IdleTimeout < 0 {
		return errors.New("http write wait must be greater or equal to zero")
	}
	return nil
}

type ProtocolIntervals struct {
	// The duration after which a message should have been written.
	// If the message was not fully written within this time frame,
	// the connection is closed.
	WriteTimeout time.Duration `json:"write_wait"`

	// The duration after which a pong message should have arrived.
	// If no pong is received within this time frame, the connection is closed.
	PongTimeout time.Duration `json:"pong_wait"`

	// The interval at which websocket ping messages are sent.
	PingInterval time.Duration `json:"ping_interval"`

	// The duration after which an expected client message should have arrived.
	// If no message is received within this time frame,
	// the connection is closed.
	ClientTimeout time.Duration `json:"timeout_duration"`

	// The interval at which client message timeouts are checked.
	ClientTimeoutInterval time.Duration `json:"timeout_interval"`
}

func (i *ProtocolIntervals) Validate() error {
	if i.WriteTimeout <= 0 {
		return errors.New("write wait must be greater than zero")
	}
	if i.PongTimeout <= 0 {
		return errors.New("pong wait must be greater than zero")
	}
	if i.PingInterval <= 0 {
		return errors.New("ping interval must be greater than zero")
	}
	if i.ClientTimeout <= 0 {
		return errors.New("timeout duration must be greater than zero")
	}
	if i.ClientTimeoutInterval <= 0 {
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
