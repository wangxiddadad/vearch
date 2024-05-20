package schema

import "github.com/vearch/vearch/sdk/go/v3/connection"

type API struct {
	connection *connection.Connection
}

func New(con *connection.Connection) *API {
	return &API{connection: con}
}

func (schema *API) DBCreator() *DBCreator {
	return &DBCreator{
		connection: schema.connection,
	}
}

func (schema *API) DBDeleter() *DBDeleter {
	return &DBDeleter{
		connection: schema.connection,
	}
}

func (schema *API) SpaceCreator() *SpaceCreator {
	return &SpaceCreator{
		connection: schema.connection,
	}
}

func (schema *API) SpaceDeleter() *SpaceDeleter {
	return &SpaceDeleter{
		connection: schema.connection,
	}
}