package frontend

import (
	"fmt"
	"net/http"
	"time"

	be "github.com/cvmfs/gateway/internal/gateway/backend"

	"github.com/gorilla/mux"
	"github.com/pkg/errors"
)

// NewFrontend builds and configures a new HTTP server, but does not start it
func NewFrontend(services *be.Services, port int, timeout time.Duration) *http.Server {
	router := mux.NewRouter()

	// Add the request tagging middleware
	router.Use(MakeTaggingMiddleware())

	// Add the HMAC authorization middleware
	router.Use(MakeAuthzMiddleware(&services.Access))

	// Register the different routes

	// Root handler
	router.Path(APIRoot).HandlerFunc(NewRootHandler())

	// Repositories
	router.Path(APIRoot + "/repos/{name}").
		Methods("GET").
		HandlerFunc(MakeReposHandler(services))
	router.Path(APIRoot + "/repos").
		Methods("GET").
		HandlerFunc(MakeReposHandler(services))

	// Leases
	router.Path(APIRoot+"/leases").
		Methods("GET", "POST").
		HandlerFunc(MakeLeasesHandler(services))
	router.Path(APIRoot+"/leases/{token}").
		Methods("GET", "DELETE", "POST").
		HandlerFunc(MakeLeasesHandler(services))

	// Payloads (legacy endpoint)
	router.Path(APIRoot + "/payloads").
		Methods("POST").
		HandlerFunc(MakePayloadsHandler(services))

	// Payloads (new and improved)
	router.Path(APIRoot + "/payloads/{token}").
		Methods("POST").
		HandlerFunc(MakePayloadsHandler(services))

	// Configure and start the HTTP server
	srv := &http.Server{
		Handler:      router,
		Addr:         fmt.Sprintf(":%d", port),
		WriteTimeout: timeout,
		ReadTimeout:  timeout,
	}

	return srv
}

// Start HTTP frontend
func Start(services *be.Services, port int, timeout time.Duration) error {
	srv := NewFrontend(services, port, timeout)
	if err := srv.ListenAndServe(); err != nil {
		return errors.Wrap(err, "could not run HTTP front-end")
	}

	return nil
}
