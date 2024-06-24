package client

import (
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"

	"github.com/ungive/loon/pkg/client"
	"github.com/ungive/loon/pkg/server"
)

var Cmd = flag.NewFlagSet("client", flag.ExitOnError)

var (
	addr        = Cmd.String("server", "", "server address")
	auth        = Cmd.String("auth", "", "HTTP basic authentication string")
	contentType = Cmd.String("type", "", "explicitly set the HTTP content type")
	attachment  = Cmd.String("download", "", "set the download filename")
	help        = Cmd.Bool("help", false, "print help")
)

func init() {
	log.SetFlags(0)
}

func Usage(cmd string) {
	fmt.Println(cmd + " -server <address> [options] <path> [<path> ...]")
	Cmd.PrintDefaults()
}

func Main(cmd string, args []string) {
	Cmd.Parse(args)
	if *help {
		Usage(cmd)
		return
	}
	sanitizeArgs()
	cli := runClient(*addr, auth)
	defer cli.Close()
	registerPaths(cli, Cmd.Args())
	waitForExit()
}

func sanitizeArgs() {
	if len(*addr) == 0 || Cmd.NArg() == 0 || len(Cmd.Arg(0)) == 0 {
		log.Fatalf("invalid arguments")
	}
	if Cmd.NArg() > 1 && len(*contentType) > 0 {
		log.Fatalf("cannot set content type explicitly with multiple paths")
	}
	if Cmd.NArg() > 1 && len(*attachment) > 0 {
		log.Fatalf("cannot set download filename with multiple paths")
	}
	duplicates := make(map[string]struct{})
	for _, path := range Cmd.Args() {
		base := filepath.Base(path)
		_, ok := duplicates[base]
		if ok {
			log.Fatalf("duplicate basename: %v", base)
		}
		duplicates[base] = struct{}{}
	}
	if len(*auth) == 0 {
		auth = nil
	} else {
		user, pass, ok := strings.Cut(*auth, ":")
		if !ok {
			log.Fatalf("invalid basic auth string: expected colon")
		}
		if len(user) == 0 {
			log.Fatalf("invalid basic auth string: user cannot be empty")
		}
		if len(pass) == 0 {
			log.Fatalf("invalid basic auth string: password cannot be empty")
		}
	}
}

func runClient(baseUrl string, httpBasicAuth *string) client.Client {
	cli, err := client.NewClient(baseUrl, httpBasicAuth)
	if err != nil {
		log.Fatalf("failed to create new client: %v", err)
	}
	go func() {
		err := cli.Run()
		if err != nil {
			log.Fatalf("Run() exited with error: %v", err)
		} else {
			log.Println("Run() exited")
		}
		os.Exit(0)
	}()
	return cli
}

func registerPaths(cli client.Client, paths []string) {
	var explicitContentType *server.ContentType
	if len(*contentType) > 0 {
		var err error
		explicitContentType, err = server.NewContentType(*contentType)
		if err != nil {
			log.Fatalf("failed to parse content type: %v", err)
		}
	}
	var filename *string
	if len(*attachment) > 0 {
		filename = attachment
	}
	for _, path := range paths {
		file, err := os.Open(path)
		if err != nil {
			log.Fatalf("failed to open file: %v", err)
		}
		source, err := client.NewFileContentSource(file, explicitContentType)
		if err != nil {
			log.Fatalf("failed to create file content source: %v", err)
		}
		content, err := cli.Register(source, &client.ContentInfo{
			Path:               filepath.Base(file.Name()),
			AttachmentFilename: filename,
			MaxCacheDuration:   0,
			UploadLimit:        0,
		})
		if err != nil {
			log.Fatalf("failed to register content: %v", err)
		}
		log.Printf("%v: %v\n", path, content.URL())
	}
}

func waitForExit() {
	// Block until the user hits CTRL+C
	done := make(chan os.Signal, 1)
	signal.Notify(done, syscall.SIGINT, syscall.SIGTERM)
	<-done
}
