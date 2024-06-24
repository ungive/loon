FROM golang:1.22-alpine

WORKDIR /app

COPY go.mod go.sum ./
RUN go mod download

COPY cmd ./cmd
COPY pkg ./pkg
COPY internal ./internal
RUN CGO_ENABLED=0 GOOS=linux go build -o /loon ./cmd/loon

VOLUME /app/config.yaml
CMD [ "/loon", "server", "-addr", ":80", "-config", "/app/config.yaml" ]
