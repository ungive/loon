.PHONY: proto build-image publish-image

all:

proto:
	protoc -I=. --go_out=pkg ./api/messages.proto

build-image:
	docker build -t loon -f build/package/Dockerfile .

publish-image: build-image
	docker tag loon ghcr.io/ungive/loon:latest
	docker push ghcr.io/ungive/loon:latest
