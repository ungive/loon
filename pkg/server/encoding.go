package server

import (
	"encoding/base64"
)

// Common functions for encoding a list of bytes for representation in a URL.
// Used for encoding both client IDs and MAC hashes.

const encodingAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"

var urlEncoding = base64.NewEncoding(encodingAlphabet).WithPadding(base64.NoPadding)

func UrlEncodeBytes(data []byte) string {
	return urlEncoding.EncodeToString(data)
}

func UrlDecodeBytes(data string) ([]byte, error) {
	return urlEncoding.DecodeString(data)
}
