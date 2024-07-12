# Deployments

## Docker Compose

Features:
- Caddy reverse proxy
- HTTP basic authentication for the `/ws` websocket endpoint

### Environment variables

#### Caddy

Create `caddy.env` from `caddy.env.example`.
The example password is `hiccup`, taken from the
[docs](https://caddyserver.com/docs/caddyfile/directives/basic_auth).

Hash your own password:

```sh
$ openssl rand -base64 15
VWY6hUaKUg4ZlQ0c0OfL
$ caddy hash-password -p "VWY6hUaKUg4ZlQ0c0OfL"
$2a$14$u8EDmHIMQYomhA3MIAdefuQSshJkDZQa4w2iULE.f/EahnDmdA.Pu
```

Put it into `caddy.env` and replace `$` with `$$` to escape dollar signs.

Create a self-signed certificate for websocket clients:

```sh
$ make cert
```

With a self-signed certificate clients will be able to authenticate
that they are talking to the server they want to talk to.
A certificate that is trusted by browsers
is not necessary for websocket clients.
Only the HTTPS endpoint will need such a certificate.
This is reflected in the Caddyfile.

#### Usage

Start the containers:

```sh
docker compose up -d
```

Reload the config:

```sh
docker compose exec -w /etc/caddy caddy caddy reload
```

Documentation: https://caddyserver.com/docs/running#docker-compose

Use the password with the Go client:

```
loon client -server http://localhost:8080 -auth loon-client:hiccup assets/loon-small.png
```
