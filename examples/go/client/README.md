# Example Go client

Assuming a test server is running with e.g.:

```sh
# in the root directory of the project:
go run ./cmd/loon server
```

Server a local file with:

```sh
# in this directory:
go build main.go
go run main.go -server http://localhost:8080 ../../../assets/loon-full.png
```
