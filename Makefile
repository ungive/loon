.PHONY: proto

all:

proto:
	protoc -I=. --go_out=pkg ./api/messages.proto
