#!/bin/bash
# Pixi activation script for aegir.
# Sourced automatically by `pixi run` / `pixi shell`.

export AEGIR_ROOT="$PIXI_PROJECT_ROOT"

# Locally built plugins first, then installed plugins from the pixi env.
export PHLEX_PLUGIN_PATH="$PIXI_PROJECT_ROOT/build:${CONDA_PREFIX}/lib${PHLEX_PLUGIN_PATH:+:$PHLEX_PLUGIN_PATH}"
export LD_LIBRARY_PATH="$PIXI_PROJECT_ROOT/build${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# shipgeometry installs geometry DBs under $CONDA_PREFIX/share/geometry/.
# geometry_geomodel_provider resolves bare db_file names via this variable.
export SHIPGEOMETRY_ROOT="${SHIPGEOMETRY_ROOT:-$CONDA_PREFIX}"

# SHiPFieldService resolves bare .cvf filenames via $SHIPFIELD_ROOT/share/field/.
# Kept distinct from SHIPGEOMETRY_ROOT so field maps and geometry can be
# versioned independently.
export SHIPFIELD_ROOT="${SHIPFIELD_ROOT:-$CONDA_PREFIX}"

# gmex (GeoModelExplorer) workaround: in conda-forge geomodel-visualization
# 6.27.0 the install prefix is baked in as a NUL-padded literal via binary
# prefix replacement, so derived paths truncate at $CONDA_PREFIX and gmex
# tries to ifstream the env directory itself -> SIGABRT. GXSHAREDIR is checked
# before the baked-in path, so pointing it at the real share dir restores
# normal startup. Drop once a fixed feedstock build is available.
export GXSHAREDIR="${GXSHAREDIR:-$CONDA_PREFIX/share/gmex}"
