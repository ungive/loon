package server

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"strings"
)

const (
	MacPathPrefix  = "/"
	MacQueryPrefix = "?"
)

type MAC []byte

func ComputeMac(
	clientId string,
	path string,
	query string,
	clientSecret []byte,
) (MAC, error) {
	path = strings.TrimLeft(path, MacPathPrefix)
	query = strings.TrimLeft(query, MacQueryPrefix)
	mac := hmac.New(sha256.New, clientSecret)
	items := [][]byte{
		[]byte(clientId),
		[]byte(MacPathPrefix),
		[]byte(path),
	}
	if len(query) > 0 {
		items = append(items, []byte(MacQueryPrefix))
		items = append(items, []byte(query))
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
