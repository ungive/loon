package server

import (
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"io"
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

func NewUUID() (UUID, error) {
	var id UUID = UUID{}
	_, err := io.ReadFull(rand.Reader, id[:])
	if err != nil {
		return UUID{}, err
	}
	return id, nil
}

func ParseUUID(s string) (UUID, error) {
	data, err := hex.DecodeString(s)
	if err != nil {
		return UUID{}, err
	}
	var id UUID
	copy(id[:], data)
	return id, nil
}

func (c UUID) String() string {
	return hex.EncodeToString(c[:])
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
