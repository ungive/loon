.PHONY: proto

all:

proto:
	protoc -I=. --go_out=pkg ./api/messages.proto

build-image:
	docker build -t ungive/loon:latest -f build/package/Dockerfile .

publish-image:
	docker push ungive/loon:latest
