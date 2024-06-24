.PHONY: proto

all:

proto:
	protoc -I=. --go_out=pkg ./api/messages.proto

docker-image:
	docker build -t loon:latest -f build/package/Dockerfile .
