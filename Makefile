.PHONY: proto

all:

proto:
	protoc -I=. --go_out=server ./protocol/messages.proto
