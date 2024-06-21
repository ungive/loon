package main

import (
	"flag"
	"log/slog"
	"net/http"
	"os"
	"path/filepath"

	"github.com/goccy/go-yaml"
	"github.com/ungive/loon/pkg/server"
)

var (
	defaultPort = "8080"
	defaultAddr = ":" + defaultPort
	addr        = flag.String("addr", defaultAddr, "http service address")
	configPath  = flag.String("config", "", "the path to a config file")
)

var log *slog.Logger

func main() {
	flag.Parse()
	config := readConfig(*configPath)
	server, err := server.NewServer(config, log)
	go server.Run()
	if err != nil {
		log.Error("failed to start server", "err", err)
		abort()
	}
	srv := http.Server{
		Addr:         *addr,
		WriteTimeout: config.Http.WriteWait,
		Handler:      server.Handler(),
	}
	err = srv.ListenAndServe()
	if err != nil {
		log.Error("failed to listen on address", "addr", *addr, "err", err)
		abort()
	}
}

func readConfig(path string) *server.Config {
	log = newLogger(slog.LevelWarn)
	clog := log.With("context", "config")
	if len(*configPath) <= 0 {
		if *addr != defaultAddr {
			clog.Error("can only use the default config" +
				" with the default address. please supply a config path" +
				" or use the default address for testing")
			abort()
		}
		clog.Warn("using the default config, only do this during testing")
		return defaultConfig
	}
	absPath, err := filepath.Abs(path)
	if err != nil {
		clog.Error("failed to retrieve absolute path of config", "err", err)
		abort()
	}
	data, err := os.ReadFile(absPath)
	if err != nil {
		clog.Error("failed to open config file", "err", err)
		abort()
	}
	var v server.Config
	if err := yaml.Unmarshal(data, &v); err != nil {
		clog.Error("failed to parse config YAML", "err", err)
		abort()
	}
	if err := v.Validate(); err != nil {
		clog.Error("failed to validate config", "err", err)
		abort()
	}
	// Create a new logger to reflect the configured logging level.
	log = newLogger(v.Log.Level)
	log.Info("loaded config", "path", absPath)
	return &v
}

func newLogger(defaultLevel slog.Level) *slog.Logger {
	return slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{
		Level: defaultLevel,
	}))
}

func abort() {
	exit_code := 1
	log.Error("Exiting", "exit_code", exit_code)
	os.Exit(exit_code)
}
