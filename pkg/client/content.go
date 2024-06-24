package client

import (
	"bufio"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"log"
	"mime"
	"net/http"
	"os"
	"path/filepath"

	"github.com/ungive/loon/pkg/server"
)

// Source for client content.
type ContentSource interface {

	// Returns an io.Reader that supplies the binary data for the content.
	// Must return a reader that points to the beginning of the data.
	Data() (io.Reader, error)

	// Returns the size of the data in bytes.
	// At least this many bytes must be supplied by the reader without error,
	// and at most this many bytes will be read from the reader.
	Size() uint64

	// Returns the content type as a valid MIME type/HTTP content type,
	// as defined in RFC 1341: https://www.rfc-editor.org/rfc/rfc1341.
	ContentType() string

	// Deallocates any allocated resources associated with this content source.
	Close()
}

type ContentInfo struct {

	// The URL path under which to make the content available.
	// Note that making content available under a path,
	// after other content was available under the same path before,
	// might cause the old content to still be available under that path
	// until all caches are cleared.
	// It is recommended to never register a path twice,
	// if the content under that path can change over time
	// for the lifetime of the client connection.
	Path string

	// Indicates that the content should be downloaded as an attachment
	// with the given filename.
	AttachmentFilename *string

	// The maximum duration for which content may be cached.
	// A zero value indicates that the content should not be stored in caches.
	MaxCacheDuration uint32

	// Upload speed limit in bytes per second.
	// Zero indicates no upload limit.
	UploadLimit uint64
}

type ContentHandle interface {
	// Returns the full HTTP/S URL under which the content is available.
	URL() string
}

type fileContentSource struct {
	file        *os.File
	info        fs.FileInfo
	contentType string
}

// Creates a new content source which reads from a file.
// The content type can be explicitly by passing a non-nil value
// for the explicit content type parameter.
// If the content type is nil, the content type will be guess from either
// the file extension or the beginning of the file's contents.
func NewFileContentSource(
	file *os.File,
	explicitContentType *server.ContentType,
) (ContentSource, error) {
	info, err := file.Stat()
	if err != nil {
		return nil, fmt.Errorf("failed to stat file: %w", err)
	}
	var contentType string
	if explicitContentType == nil {
		contentType, err = detectContentType(file)
		if err != nil {
			return nil, err
		}
	} else {
		contentType = explicitContentType.String()
	}
	return &fileContentSource{
		file:        file,
		info:        info,
		contentType: contentType,
	}, nil
}

func (s *fileContentSource) Data() (io.Reader, error) {
	_, err := s.file.Seek(0, io.SeekStart)
	if err != nil {
		return nil, err
	}
	return bufio.NewReader(s.file), nil
}

func (s *fileContentSource) Size() uint64 {
	return uint64(s.info.Size())
}

func (s *fileContentSource) ContentType() string {
	return s.contentType
}

func (s *fileContentSource) Close() {
	s.file.Close()
}

func detectContentType(file *os.File) (string, error) {
	extType := mime.TypeByExtension(filepath.Ext(file.Name()))
	if len(extType) > 0 {
		return extType, nil
	}
	_, err := file.Seek(0, io.SeekStart)
	if err != nil {
		log.Fatalf("failed to seek to beginning of file: %v", err)
	}
	defer file.Seek(0, io.SeekStart)
	reader := bufio.NewReader(file)
	buf := make([]byte, 512)
	n, err := reader.Read(buf)
	if errors.Is(err, io.EOF) {
		return "", fmt.Errorf("cannot detect content type of empty file: %w",
			err)
	}
	if err != nil {
		return "", fmt.Errorf("failed to detect content type: %w", err)
	}
	return http.DetectContentType(buf[0:n]), nil
}
