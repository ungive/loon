package server

import (
	"crypto/rand"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"log/slog"
)

const (
	size = 16
)

func init() {
	assertCryptoRandAvailable()
}

func assertCryptoRandAvailable() {
	buf := make([]byte, 1)
	_, err := io.ReadFull(rand.Reader, buf)
	if err != nil {
		panic(fmt.Sprintf("crypto/rand unavailable: %#v", err))
	}
}

type UUID [size]byte

// Creates a new, random UUID.
func NewUUID() (UUID, error) {
	var uuid UUID
	_, err := io.ReadFull(rand.Reader, uuid[:])
	if err != nil {
		return UUID{}, err
	}
	return uuid, nil
}

// Parses a UUID from a hex-encoded string.
func ParseUUID(s string) (UUID, error) {
	decoded, err := hex.DecodeString(s)
	if err != nil {
		return UUID{}, err
	}
	if len(decoded) != size {
		return UUID{}, errors.New("the encoded string has an invalid length")
	}
	var uuid [size]byte
	copy(uuid[:], decoded)
	return uuid, nil
}

// Returns the UUID as a hex-encoded string.
func (uuid UUID) String() string {
	return hex.EncodeToString(uuid[:])
}

func (uuid UUID) LogValue() slog.Value {
	return slog.StringValue(uuid.String())
}

// func ParseUUID(s string) (UUID, error) {
// 	var i big.Int
// 	_, ok := i.SetString(s, 62)
// 	if !ok {
// 		return UUID{}, fmt.Errorf("failed to parse base62: %q", s)
// 	}
// 	bytes := i.Bytes()
// 	if len(bytes) != size {
// 		return UUID{}, errors.New("the parsed string is not a valid UUID")
// 	}
// 	var id UUID
// 	copy(id[:], bytes)
// 	return id, nil
// }

// func (c UUID) String() string {
// 	var i big.Int
// 	i.SetBytes(c[:])
// 	return i.Text(62)
// }

// func ParseUUID(s string) (UUID, error) {
// 	data, err := base64.URLEncoding.DecodeString(s)
// 	if err != nil {
// 		return UUID{}, err
// 	}
// 	var id UUID
// 	copy(id[:], data)
// 	return id, nil
// }

// func (c UUID) String() string {
// 	return base64.URLEncoding.EncodeToString(c[:])
// }
