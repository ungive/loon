package main

import (
	"errors"
	"mime"
	"strings"
)

const (
	CONTENT_TYPE_SEP         = ","
	CONTENT_TYPE_PARAM_SEP   = ";"
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
		RootType: strings.TrimSpace(root),
		SubType:  strings.TrimSpace(sub),
		Params:   params,
	}, nil
}

func (c *ContentType) HasWildcard() bool {
	return c.RootType == "*" || c.SubType == "*"
}

func (c *ContentType) MatchesAll() bool {
	return c.RootType == "*" && c.SubType == "*"
}

func (c *ContentType) MatchesAllSubtypes() bool {
	return c.RootType != "*" && c.SubType == "*"
}

func (c *ContentType) String() string {
	return mime.FormatMediaType(c.Type, c.Params)
}

type ContentTypeIndex interface {
	ContentTypes() []*ContentType
	ContainsContentType(value string) bool
	ContainsRootContentType(value string) bool
}

type SingleContentTypeIndex struct {
	value *ContentType
}

func NewSingleContentTypeIndex(contentType *ContentType) *SingleContentTypeIndex {
	return &SingleContentTypeIndex{
		value: contentType,
	}
}

func (i *SingleContentTypeIndex) ContentTypes() []*ContentType {
	return []*ContentType{i.value}
}

func (i *SingleContentTypeIndex) ContainsContentType(value string) bool {
	return i.value.Type == value
}

func (i *SingleContentTypeIndex) ContainsRootContentType(value string) bool {
	return i.value.RootType == value
}

type MultiContentTypeIndex struct {
	values           []*ContentType
	contentTypes     map[string]struct{}
	rootContentTypes map[string]struct{}
}

func NewMultiContentTypeIndex(
	contentTypes []*ContentType,
) *MultiContentTypeIndex {
	index := &MultiContentTypeIndex{
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

func (i *MultiContentTypeIndex) ContentTypes() []*ContentType {
	return i.values
}

func (i *MultiContentTypeIndex) ContainsContentType(value string) bool {
	_, ok := i.contentTypes[value]
	return ok
}

func (i *MultiContentTypeIndex) ContainsRootContentType(value string) bool {
	_, ok := i.rootContentTypes[value]
	return ok
}

type ContentTypeRegistry struct {
	index ContentTypeIndex
}

func NewContentTypeRegistry(index ContentTypeIndex) *ContentTypeRegistry {
	return &ContentTypeRegistry{
		index: index,
	}
}

func NewContentTypeRegistryFromStrings(
	contentTypes []string,
) (*ContentTypeRegistry, error) {
	result := make([]*ContentType, len(contentTypes))
	for i, value := range contentTypes {
		contentType, err := NewContentType(value)
		if err != nil {
			return nil, err
		}
		result[i] = contentType
	}
	index := NewMultiContentTypeIndex(result)
	registry := NewContentTypeRegistry(index)
	return registry, nil
}

// Checks whether the given content type is in the registry.
func (r *ContentTypeRegistry) Contains(contentType *ContentType) bool {
	if contentType.MatchesAll() {
		return true
	}
	if contentType.MatchesAllSubtypes() {
		return r.index.ContainsRootContentType(contentType.RootType)
	}
	return r.index.ContainsContentType(contentType.Type)
}

// Checks whether any of the accepted content types are in the registry.
func (r *ContentTypeRegistry) CanServeAccept(acceptValue string) bool {
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
			if r.index.ContainsRootContentType(root) {
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
