# Changelog

All notable changes to the NDI Advanced Output Plugin will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.3] - 2024-05-27

### Removed
- Legacy build variants (NDIOutputSimple, NDIOutputModern)
- Legacy OFX wrapper class dependencies
- Unused build system complexity
- Old plugin installations to prevent conflicts

### Changed
- Streamlined build system to single modern implementation
- Simplified Makefile with clean targets
- Updated documentation to reflect simplified build process

### Fixed
- Removed potential plugin conflicts from multiple installed versions

## [1.0.2] - 2024-05-27

### Added
- Semantic versioning system with automatic patch increment on builds
- Version increment script (`scripts/increment_version.sh`)
- Version display in plugin description
- `make show-version` command to display current version
- `make dev` command for development builds without version increment
- Comprehensive version management documentation

### Changed
- Build system now automatically increments patch version on `make`
- Plugin description now includes version number
- Updated README with version management section

## [1.0.1] - 2024-05-27

### Added
- Initial version tracking system
- VERSION file for semantic versioning

## [1.0.0] - 2024-05-27

### Added
- Modern OFX C API implementation for maximum compatibility
- Comprehensive HDR support:
  - PQ (ST.2084) and HLG (Hybrid Log-Gamma) transfer functions
  - Rec.2020, DCI-P3, and Rec.709 color spaces
  - Configurable Max Content Light Level (CLL) and Max Frame Average Light Level (FALL)
  - HDR metadata embedding for proper downstream handling
- NDI Advanced SDK v6.1.1 integration
- Real-time streaming with pass-through design
- User-friendly parameter controls
- Professional-grade build system

### Changed
- Complete rewrite from legacy OFX wrapper classes to modern C API
- Improved compatibility with DaVinci Resolve 20+
- Enhanced performance and stability

### Removed
- Dependency on deprecated OFX C++ wrapper classes
- Legacy build system complexity

## [0.x] - Legacy Versions

### Features
- Basic NDI streaming functionality
- Original implementation using OFX wrapper classes
- Limited HDR support 