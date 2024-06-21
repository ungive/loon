package server

import (
	"flag"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"path/filepath"

	"github.com/goccy/go-yaml"
	"github.com/ungive/loon/pkg/server"
)

var Cmd = flag.NewFlagSet("server", flag.ExitOnError)
var logger = defaultLogger()

var (
	defaultPort = "8080"
	defaultAddr = ":" + defaultPort
	serviceAddr = Cmd.String("addr", defaultAddr, "http service address")
	configPath  = Cmd.String("config", "", "the path to a config file")
	help        = Cmd.Bool("help", false, "print help")
)

func Usage(cmd string) {
	fmt.Println(cmd + " [options]")
	Cmd.PrintDefaults()
}

func Main(cmd string, args []string) {
	Cmd.Parse(args)
	if *help {
		Usage(cmd)
		return
	}
	config := readConfig(*configPath)
	server, err := server.NewServer(config, logger)
	go server.Run()
	if err != nil {
		logger.Error("failed to start server", "err", err)
		abort()
	}
	srv := http.Server{
		Addr:         *serviceAddr,
		WriteTimeout: config.Http.WriteTimeout,
		ReadTimeout:  config.Http.ReadTimeout,
		IdleTimeout:  config.Http.IdleTimeout,
		Handler:      server.Handler(),
	}
	logger.Info(fmt.Sprintf("serving on %v (%v)",
		*serviceAddr, config.Protocol.BaseUrl))
	err = srv.ListenAndServe()
	if err != nil {
		logger.Error("failed to listen on address",
			"addr", *serviceAddr, "err", err)
		abort()
	}
}

func readConfig(path string) *server.Config {
	logger = newLogger(slog.LevelWarn)
	clog := logger.With("context", "config")
	if len(*configPath) <= 0 {
		if *serviceAddr != defaultAddr {
			clog.Error("can only use the default config" +
				" with the default address. please supply a config path" +
				" or use the default address for testing")
			abort()
		}
		clog.Warn("using the default config, only do this during testing")
		logger = newLogger(slog.LevelDebug)
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
	logger = newLogger(v.Log.Level)
	logger.Info("loaded config", "path", absPath)
	return &v
}

func defaultLogger() *slog.Logger {
	return newLogger(slog.LevelDebug)
}

func newLogger(defaultLevel slog.Level) *slog.Logger {
	return slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{
		Level: defaultLevel,
	}))
}

func abort() {
	exit_code := 1
	logger.Error("Exiting", "exit_code", exit_code)
	os.Exit(exit_code)
}
