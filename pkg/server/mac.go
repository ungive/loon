package server

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"strings"
)

const MacPathPrefix = "/"

type MAC []byte

func ComputeMac(
	clientId string,
	path string,
	clientSecret []byte,
) (MAC, error) {
	path = strings.TrimLeft(path, MacPathPrefix)
	mac := hmac.New(sha256.New, clientSecret)
	items := [][]byte{
		[]byte(clientId),
		[]byte(MacPathPrefix),
		[]byte(path),
	}
	for _, item := range items {
		n, err := mac.Write(item)
		if err != nil {
			return nil, err
		}
		if n != len(item) {
			return nil, errors.New("write did not write all bytes")
		}
	}
	return mac.Sum(nil), nil
}

func UrlDecodeMAC(s string) (MAC, error) {
	return UrlDecodeBytes(s)
}

func (mac MAC) UrlEncode() string {
	return UrlEncodeBytes(mac)
}

func (mac MAC) String() string {
	return hex.EncodeToString(mac)
}
