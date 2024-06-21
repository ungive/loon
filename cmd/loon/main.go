package main

import (
	"flag"
	"fmt"
	"os"

	"github.com/ungive/loon/cmd/loon/client"
	"github.com/ungive/loon/cmd/loon/server"
)

var (
	cmd  = flag.NewFlagSet("loon", flag.ExitOnError)
	help = cmd.Bool("help", false, "print help")
)

func usage() {
	fmt.Println(cmd.Name())
	cmd.PrintDefaults()
	fmt.Println()
	server.Usage(cmdName(server.Cmd))
	fmt.Println()
	client.Usage(cmdName(client.Cmd))
}

func main() {
	cmd.Parse(os.Args[1:])
	if *help || len(os.Args) < 2 {
		usage()
		return
	}
	switch os.Args[1] {
	case server.Cmd.Name():
		server.Main(cmdName(server.Cmd), os.Args[2:])
	case client.Cmd.Name():
		client.Main(cmdName(client.Cmd), os.Args[2:])
	default:
		usage()
	}
}

func cmdName(flagSet *flag.FlagSet) string {
	return cmd.Name() + " " + flagSet.Name()
}
