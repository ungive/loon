.PHONY: all docs

all:

docs:
	doxygen Doxyfile

run-test-server:
	go run ../../cmd/loon server -addr localhost:8071 -config ./test/server.config.yml
