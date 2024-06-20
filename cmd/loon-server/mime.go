package main

import (
	"errors"
	"mime"
	"strings"
)

const (
	CONTENT_TYPE_SEP         = ","
	CONTENT_TYPE_SUBTYPE_SEP = "/"
)

var ErrInvalidContentType = errors.New("invalid content type")

type ContentType struct {
	Type     string
	RootType string
	SubType  string
	Params   map[string]string
}

func NewContentType(value string) (*ContentType, error) {
	parsed, params, err := mime.ParseMediaType(value)
	if err != nil {
		return nil, err
	}
	root, sub, found := strings.Cut(parsed, CONTENT_TYPE_SUBTYPE_SEP)
	if !found {
		return nil, ErrInvalidContentType
	}
	return &ContentType{
		Type:     parsed,
		RootType: root,
		SubType:  sub,
		Params:   params,
	}, nil
}

type ContentTypeIndex interface {
	ContentTypes() []*ContentType
	ContainsContentType(value string) bool
	ContainsRootContentType(value string) bool
}

type singleContentTypeIndex struct {
	value *ContentType
}

func newSingleContentTypeIndex(contentType *ContentType) *singleContentTypeIndex {
	return &singleContentTypeIndex{
		value: contentType,
	}
}

func (i *singleContentTypeIndex) ContentTypes() []*ContentType {
	return []*ContentType{i.value}
}

func (i *singleContentTypeIndex) ContainsContentType(value string) bool {
	return i.value.Type == value
}

func (i *singleContentTypeIndex) ContainsRootContentType(value string) bool {
	return i.value.RootType == value
}

type multiContentTypeIndex struct {
	values           []*ContentType
	contentTypes     map[string]struct{}
	rootContentTypes map[string]struct{}
}

func newMultiContentTypeIndex(
	contentTypes []*ContentType,
) *multiContentTypeIndex {
	index := &multiContentTypeIndex{
		values:           make([]*ContentType, len(contentTypes)),
		contentTypes:     make(map[string]struct{}),
		rootContentTypes: make(map[string]struct{}),
	}
	for i, value := range contentTypes {
		index.values[i] = value
		index.contentTypes[value.Type] = struct{}{}
		index.rootContentTypes[value.RootType] = struct{}{}
	}
	return index
}

func (i *multiContentTypeIndex) ContentTypes() []*ContentType {
	return i.values
}

func (i *multiContentTypeIndex) ContainsContentType(value string) bool {
	_, ok := i.contentTypes[value]
	return ok
}

func (i *multiContentTypeIndex) ContainsRootContentType(value string) bool {
	_, ok := i.rootContentTypes[value]
	return ok
}

type contentTypeRegistry struct {
	index ContentTypeIndex
}

func newContentTypeRegistry(index ContentTypeIndex) *contentTypeRegistry {
	return &contentTypeRegistry{
		index: index,
	}
}

// Checks whether any of the accepted content types are in the registry.
func (r *contentTypeRegistry) canServeAccept(acceptValue string) bool {
	acceptedContentTypes := strings.Split(acceptValue, CONTENT_TYPE_SEP)
	for _, contentType := range acceptedContentTypes {
		index := strings.Index(contentType, ";")
		if index >= 0 {
			contentType = contentType[:index]
		}
		contentType = strings.TrimSpace(contentType)
		root, sub, found := strings.Cut(contentType, "/")
		if !found || len(root) == 0 || len(sub) == 0 {
			continue // invalid
		}
		if sub == "*" {
			if root == "*" {
				return true
			}
			if r.index.ContainsContentType(root) {
				return true
			}
			continue
		}
		if root == "*" {
			continue // invalid
		}
		if r.index.ContainsContentType(contentType) {
			return true
		}
	}
	return false
}
