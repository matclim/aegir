# aegir

Phlex-based simulation framework for the SHiP experiment.

## Architecture

```
[Source: ParticleGun / Pythia8 / FixedTarget] -> std::vector<MCParticle>
    |
[Provider: IGeometrySource]
    |
[Transform: Geant4] -> SimResult {hits, particles}
    |
[Observer: RNTupleWriter + Histogrammer] -> output.root, validation.root
```

### Plugins

| Plugin | Type | Description |
|--------|------|-------------|
| `particle_gun_source` | Source | Configurable single-particle gun with Random123 RNG |
| `pythia8_source` | Source | Pythia8 fixed-target p-p (serial or PythiaParallel) |
| `fixed_target_source` | Source | Dual-target Pythia8 (p-p/p-n) with interaction point sampling |
| `geometry_builtin_provider` | Provider | W target + Si scoring planes (test geometry) |
| `geometry_gdml_provider` | Provider | GDML file loader |
| `geometry_geomodel_provider` | Provider | GeoModel .db via SHiPGeometryService (optional) |
| `geant4_module` | Transform | Geant4 simulation (direct worker, configurable concurrency) |
| `sim_output_module` | Observer | RNTuple parallel writer + validation histograms |

## Dependencies

- [SHiPDataModel](https://github.com/ShipSoft/data-model) (event data classes)
- [Phlex](https://github.com/Framework-R-D/phlex) (+ Boost, TBB, spdlog)
- ROOT 6.36+ (Core, RIO, Hist, MathCore, ROOTNTuple)
- Geant4 11.4+ (with multi-threading enabled)
- Pythia8 8.3+
- Random123
- Optional: SHiPGeometryService (for GeoModel geometry)

## Building with pixi (recommended)

[Install pixi](https://pixi.sh) and run:

```bash
pixi install              # resolve dependencies from prefix.dev/ship + conda-forge
pixi run build            # configure + build
pixi run install          # install into the pixi environment prefix
pixi run smoke            # quick end-to-end check
```

`pixi shell` drops you into an interactive shell with the environment activated;
`PHLEX_PLUGIN_PATH`, `LD_LIBRARY_PATH`, and `SHIPGEOMETRY_ROOT` are set
automatically (see [`activate.sh`](activate.sh)). The `just` benchmark targets
are available in `pixi shell -e bench`.

## Building manually (fallback)

If you cannot use pixi (e.g. on systems still managed by aliBuild), the project
is a plain CMake build:

```bash
cmake -B build
cmake --build build
```

Pass `-DSHiPDataModel_ROOT=/path/to/data-model/install` if not in the default search path.

## Running

```bash
export PHLEX_PLUGIN_PATH="$PWD/build:$PHLEX_PLUGIN_PATH"   # not needed under pixi

# Particle gun only (no Geant4)
phlex -c workflows/gun_only.jsonnet

# Particle gun -> Geant4 (single-threaded, concurrency defaults to 1)
phlex -c workflows/gun_st.jsonnet

# Particle gun -> Geant4 (multi-threaded, concurrency=4)
phlex -c workflows/gun_mt.jsonnet

# Pythia8 -> Geant4
phlex -c workflows/pythia8_st.jsonnet

# Fixed-target -> Geant4 MT
phlex -c workflows/fixed_target_mt.jsonnet
```

## Geometry

Three geometry providers are available, selectable via workflow configuration:

- **Builtin**: Tungsten target + Si scoring planes (default)
- **GDML**: Load geometry from a GDML file
- **GeoModel**: Load from a GeoModel `.db` file via SHiPGeometryService

## Geant4 integration

See [docs/geant4_integration.md](docs/geant4_integration.md) for details on the direct worker integration strategy.

## Benchmarking

See `docs/benchmarks.md` for details. Quick start:

```bash
just bench-geant4          # ST vs MT comparison
just bench-geant4-threads  # Thread count sweep
just bench-pythia8         # Standalone vs Phlex Pythia8
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for details on our development workflow and coding standards.

## Known issues

- Geant4 bundles its own zlib, which conflicts with ROOT's system zlib. Build Geant4 with `-DGEANT4_USE_SYSTEM_ZLIB=ON`.
- G4RunManager destruction accesses global singletons that may already be torn down during plugin unloading. The RunManager is intentionally leaked.

## Licence

LGPL-3.0-or-later. See `LICENSES/` for the full text.
