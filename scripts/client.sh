#!/bin/bash
cd $(dirname $0)/..
go run ./cmd/loon client -server http://localhost:8080 $@
