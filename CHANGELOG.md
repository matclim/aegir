# Changelog

All notable changes to this project will be documented in this file.

## [0.2.0] - 2026-06-18

### Features

- Resolve geometry DB path via SHIPGEOMETRY_ROOT
- *(ci)* Add pixi-based build and CI workflow
- *(bench)* Geant4 tracing, flamegraph, and saturated sweep tooling
- *(field)* Integrate SHiPFieldService magnetic field service

### Bug fixes

- *(workflows)* Use bare db_file name in remaining geomodel workflows
- Improve SHIPGEOMETRY_ROOT path resolution
- *(deps)* Use libjsonnet instead of broken jsonnet CLI package
- *(trace)* Emit kernel TIDs and thread-name metadata
- *(field)* Error on unmatched volume_pattern; drop unused field decl
- *(deps)* Pin root_cxx_standard==23 to match aegir's C++ standard

### Refactor

- *(sim_output)* Switch validation I/O to ROOT 7 RFile + RHist
- *(workflows)* Parameterise gun_st_geomodel event count via extVar

### Documentation

- *(readme)* Expand pixi usage guide
- Contributing — point at pixi for the dev environment

### Styling

- Pre-commit fixes
- Pre-commit fixes

### Testing

- *(field)* Add smoke workflows exercising covfie + per-region wiring

### Miscellaneous

- Add LICENSE.md symlink for GitHub detection
- Use copy instead of symlink for LICENSE.md
- *(docs)* Fix link to phlex in readme
- Use ShipSoft/.github reusable workflows
- Update pixi lock file
- Add project information to CMake
## [0.1.0] - 2026-05-06
