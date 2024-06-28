#!/bin/bash
cd $(dirname $0)/..
go run ./cmd/loon server -addr localhost:8080 -config examples/server/config.yaml | hl -F
