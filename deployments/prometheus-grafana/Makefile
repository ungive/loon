.PHONY: all caddy

all:

format:
	caddy fmt --overwrite

cert:
	openssl ecparam -name prime256v1 -genkey -noout -out privkey.pem
	openssl ec -in privkey.pem -pubout -out pubkey.pem
	openssl req -new -x509 -key privkey.pem -out cert.pem -days 360
