package main

import (
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
)

const (
	ServerPort = 8071
	ProxyPort  = 8072
)

func main() {
	_, currentFile, _, ok := runtime.Caller(0)
	if !ok {
		log.Fatal("failed to get caller directory")
	}
	dir := filepath.Dir(currentFile)

	go func() {
		cmd := exec.Command(
			"go", "run", filepath.Join(dir, "../../../cmd/loon"),
			"server",
			"-addr", fmt.Sprintf("127.0.0.1:%d", ServerPort),
			"-config", filepath.Join(dir, "config.yml"),
		)
		stdout, _ := cmd.StdoutPipe()
		stderr, _ := cmd.StderrPipe()
		if err := cmd.Start(); err != nil {
			log.Fatalf("server error: %v", err)
		}
		go io.Copy(os.Stdout, stdout)
		go io.Copy(os.Stderr, stderr)
		if err := cmd.Wait(); err != nil {
			log.Fatalf("server error: %v", err)
		}
		log.Printf("server exited")
	}()

	wp, err := NewProxy(fmt.Sprintf("ws://127.0.0.1:%d/ws", ServerPort), nil)
	if err != nil {
		log.Fatalf("%v", err)
	}
	http.HandleFunc("/proxy/ws", wp.Proxy)
	http.HandleFunc("/proxy/drop_all", func(writer http.ResponseWriter, request *http.Request) {
		wp.Drop(DropAll)
		writer.WriteHeader(http.StatusOK)
	})
	http.HandleFunc("/proxy/drop_active", func(writer http.ResponseWriter, request *http.Request) {
		wp.Drop(DropActive)
		writer.WriteHeader(http.StatusOK)
	})
	http.HandleFunc("/proxy/drop_active_in", func(writer http.ResponseWriter, request *http.Request) {
		wp.Drop(DropActiveIn)
		writer.WriteHeader(http.StatusOK)
	})
	http.HandleFunc("/proxy/drop_active_out", func(writer http.ResponseWriter, request *http.Request) {
		wp.Drop(DropActiveOut)
		writer.WriteHeader(http.StatusOK)
	})
	http.HandleFunc("/proxy/drop_none", func(writer http.ResponseWriter, request *http.Request) {
		wp.Drop(DropNone)
		writer.WriteHeader(http.StatusOK)
	})
	http.HandleFunc("/proxy/close_remotes", func(writer http.ResponseWriter, request *http.Request) {
		wp.CloseRemotes()
		writer.WriteHeader(http.StatusOK)
	})
	http.ListenAndServe(fmt.Sprintf("127.0.0.1:%d", ProxyPort), nil)
}
