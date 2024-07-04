package server_test

import (
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/ungive/loon/pkg/server"
)

func Test_validating_constraints_fails_when_an_accepted_content_type_has_parameters(t *testing.T) {
	c := newConfigConstraints()
	c.AcceptedContentTypes =
		append(c.AcceptedContentTypes, "text/html; charset=utf-8")
	err := c.Validate()
	assert.NotNil(t, err)
}

func Test_validating_constraints_fails_when_a_content_type_contains_spaces(t *testing.T) {
	c := newConfigConstraints()
	c.AcceptedContentTypes =
		append(c.AcceptedContentTypes, " text/html")
	err := c.Validate()
	assert.NotNil(t, err)
}

func Test_validating_constraints_fails_when_a_content_type_is_not_all_lowercase(t *testing.T) {
	c := newConfigConstraints()
	c.AcceptedContentTypes =
		append(c.AcceptedContentTypes, "text/HTML")
	err := c.Validate()
	assert.NotNil(t, err)
}

func newConfigConstraints() *server.ProtocolConstraints {
	return &server.ProtocolConstraints{
		ChunkSize:      128,
		MaxContentSize: 128,
		AcceptedContentTypes: []string{
			"text/html",
		},
		ResponseCaching: boolPtr(false),
	}
}

func boolPtr(v bool) *bool {
	return &v
}
