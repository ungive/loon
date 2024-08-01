package server

import (
	"errors"
	"fmt"
	"log/slog"
	"mime"
	"time"

	"github.com/fatih/structs"
	"github.com/ungive/loon/pkg/pb"
)

var ErrUnsetConfigFields = errors.New("config has unset fields")

// Root object for any server configuration file.
type Options struct {
	Protocol *ProtocolOptions `json:"protocol"`
	Http     *HttpOptions     `json:"http"`
	Log      *LogOptions      `json:"log" structs:"-"`
}

type ProtocolOptions struct {
	BaseUrl         string               `json:"base_url"`
	ChunkBufferSize int                  `json:"chunk_buffer_size"`
	Constraints     *ProtocolConstraints `json:"constraints"`
	Intervals       *ProtocolIntervals   `json:"intervals"`
}

type ProtocolConstraints struct {
	ChunkSize            uint64         `json:"chunk_size"`
	MaxContentSize       uint64         `json:"max_content_size"`
	AcceptedContentTypes []string       `json:"accepted_content_types"`
	CacheDuration        *time.Duration `json:"cache_duration"`
}

func (c *ProtocolConstraints) CacheDurationInt() uint32 {
	return max(0, uint32(c.CacheDuration.Round(time.Second).Seconds()))
}

func (c *ProtocolConstraints) Proto() *pb.Constraints {
	return &pb.Constraints{
		ChunkSize:            c.ChunkSize,
		MaxContentSize:       c.MaxContentSize,
		AcceptedContentTypes: c.AcceptedContentTypes,
		CacheDuration:        c.CacheDurationInt(),
	}
}

type HttpOptions struct {
	// Write timeout. A zero value indicates no timeout (default).
	WriteTimeout time.Duration `json:"write_timeout"`
	// Read timeout. A zero value indicates no timeout (default).
	ReadTimeout time.Duration `json:"read_timeout"`
	// Idle timeout. A zero value indicates no timeout (default).
	IdleTimeout time.Duration `json:"idle_timeout"`
}

type LogOptions struct {
	Level *slog.Level `json:"level"`
}

func (c *Options) Validate() error {
	if err := c.Protocol.Validate(); err != nil {
		return fmt.Errorf("failed to validate protocol options: %w", err)
	}
	if err := c.Http.Validate(); err != nil {
		return fmt.Errorf("failed to validate http options: %w", err)
	}
	if structs.HasZero(c.Log) {
		return ErrUnsetConfigFields
	}
	return nil
}

func (c *ProtocolOptions) Validate() error {
	if len(c.BaseUrl) == 0 {
		return errors.New("base URL cannot be empty")
	}
	if c.ChunkBufferSize <= 0 {
		return errors.New("chunk buffer size must be larger than zero")
	}
	if err := c.Intervals.Validate(); err != nil {
		return fmt.Errorf("failed to validate intervals: %w", err)
	}
	if err := c.Constraints.Validate(); err != nil {
		return fmt.Errorf("failed to validate constraints: %w", err)
	}
	if structs.HasZero(c) {
		return ErrUnsetConfigFields
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

func (c *ProtocolConstraints) Validate() error {
	if c.MaxContentSize == 0 {
		return errors.New("maximum content size cannot be zero")
	}
	if c.ChunkSize > c.MaxContentSize {
		return errors.New(
			"chunk size cannot be larger than maximum content size")
	}
	if len(c.AcceptedContentTypes) == 0 {
		return errors.New("accepted content types cannot be empty")
	}
	for _, contentType := range c.AcceptedContentTypes {
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
	if *c.CacheDuration < 0 {
		return errors.New("maximum cache duration must be positive")
	}
	seconds := c.CacheDuration.Seconds()
	if seconds-float64(uint64(seconds)) > 0.001 {
		return errors.New("maximum cache duration must be in seconds")
	}
	if structs.HasZero(c) {
		return ErrUnsetConfigFields
	}
	return nil
}
