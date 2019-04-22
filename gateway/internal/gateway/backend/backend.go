package backend

import (
	gw "github.com/cvmfs/gateway/internal/gateway"
	"github.com/pkg/errors"
)

// Services is a container for the various
// backend services
type Services struct {
	Access AccessConfig
	Leases LeaseDB
	Config gw.Config
}

// Start initializes the various backend services
func Start(cfg *gw.Config) (*Services, error) {
	ac, err := NewAccessConfig(cfg.AccessConfigFile)
	if err != nil {
		return nil, errors.Wrap(
			err, "loading repository access configuration failed")
	}

	leaseDBType := "embedded"
	if cfg.UseEtcd {
		leaseDBType = "etcd"
	}
	ldb, err := OpenLeaseDB(leaseDBType, cfg)
	if err != nil {
		return nil, errors.Wrap(err, "could not create lease DB")
	}

	return &Services{Access: *ac, Leases: ldb, Config: *cfg}, nil
}

// Stop all the backend services
func (s *Services) Stop() error {
	if err := s.Leases.Close(); err != nil {
		return errors.Wrap(err, "could not close lease database")
	}
	return nil
}
