package server

import (
	"log/slog"
	"time"

	"github.com/ungive/loon/pkg/server"
)

// Sane default values for testing the server without an explicit config.
var defaultConfig = &server.Options{
	Protocol: &server.ProtocolOptions{
		BaseUrl: "localhost:" + defaultPort,
		Constraints: &server.ProtocolConstraints{
			MaxContentSize:       64 * 1024 * 1024, // 64 MiB
			ChunkSize:            64 * 1024,        // 64 KiB
			AcceptedContentTypes: defaultContentTypes,
			MaxCacheDuration:     durationPtr(time.Duration(0)), // no caching by default
		},
		Intervals: &server.ProtocolIntervals{
			WriteTimeout:          10 * time.Second,
			PongTimeout:           60 * time.Second,
			PingInterval:          48 * time.Second,
			ClientTimeout:         30 * time.Second,
			ClientTimeoutInterval: 8 * time.Second,
		},
	},
	Http: &server.HttpOptions{
		WriteTimeout: 10 * time.Second,
		ReadTimeout:  10 * time.Second,
		IdleTimeout:  30 * time.Second,
	},
	Log: &server.LogOptions{
		Level: logLevelPtr(slog.LevelDebug),
	},
}

func logLevelPtr(level slog.Level) *slog.Level {
	return &level
}

func durationPtr(v time.Duration) *time.Duration {
	return &v
}

// Some common MIME types/HTTP content types. Not exhaustive.
// Source: https://stackoverflow.com/a/48704300/6748004
var defaultContentTypes = []string{
	// application
	"application/java-archive",
	"application/octet-stream",
	"application/ogg",
	"application/pdf",
	"application/xhtml+xml",
	"application/json",
	"application/ld+json",
	"application/xml",
	"application/zip",
	// audio
	"audio/mpeg",
	"audio/x-ms-wma",
	"audio/vnd.rn-realaudio",
	"audio/x-wav",
	// image
	"image/gif",
	"image/jpeg",
	"image/png",
	"image/tiff",
	"image/vnd.microsoft.icon",
	"image/x-icon",
	"image/vnd.djvu",
	"image/svg+xml",
	// text
	"text/css",
	"text/csv",
	"text/html",
	"text/javascript",
	"text/plain",
	"text/xml",
	// video
	"video/mpeg",
	"video/mp4",
	"video/quicktime",
	"video/x-ms-wmv",
	"video/x-msvideo",
	"video/x-flv",
	"video/webm",
}
