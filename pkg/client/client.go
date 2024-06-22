package client

import "io"

// options:
// - upload speed limit
// - compression
// -

type Content interface {

	// Returns an io.Reader that supplies the binary data for the content.
	// Must return a reader that points to the beginning of the data.
	Data() io.Reader

	// Returns the size of the data in bytes.
	Size() uint64

	// Returns the content type as a valid MIME type/HTTP content type,
	// as defined in RFC 1341: https://www.rfc-editor.org/rfc/rfc1341.
	ContentType() string
}

type ContentOptions struct {
	Path string // ex: cover.jpg
}

type Client interface {

	// path:  "14becabf8915c4ea.jpg"
	// data:  (data source)
	// returns the URL under which the data is available
	MakeAvailable(path string, content Content) string
}
