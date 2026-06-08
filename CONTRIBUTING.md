# Contributing to aegir

Thank you for your interest in contributing to aegir! As part of the SHiP Collaboration, we follow a set of standards to ensure code quality and maintainability.

## Development Workflow

1. **Fork and Clone**: Create a fork of the repository and clone it locally.
2. **Environment**: Install [pixi](https://pixi.sh) — it provisions all build and runtime dependencies (ROOT, Geant4, Pythia8, Phlex, SHiPDataModel, SHiPGeometryService, …) from `conda-forge` and [`prefix.dev/ship`](https://prefix.dev/channels/ship). Then `pixi install` once to materialise the environment. Direct CMake builds against a hand-rolled dependency tree are also supported (see [README.md → Building manually](README.md#building-manually)).
3. **Pre-commit Hooks**: We use `pre-commit` to enforce coding standards. Install the hooks before making changes:
   ```bash
   pre-commit install
   ```
4. **Branching**: Create a feature branch for your changes.
5. **Coding Standards**:
   - Follow the existing C++ style (enforced by `clang-format` and `cpplint`).
   - Use `ruff` for Python script formatting.
   - Ensure all files have the correct SPDX license headers (REUSE compliant).
6. **Commits**: We follow [Conventional Commits](https://www.conventionalcommits.org/). This helps in automated changelog generation.
   - `feat: ...` for new features
   - `fix: ...` for bug fixes
   - `docs: ...` for documentation changes
   - `style: ...` for formatting
   - `refactor: ...` for code refactoring
7. **Testing**:
   - Build and run a sample workflow inside the pixi environment:
     ```bash
     pixi run build
     pixi run phlex -c workflows/gun_only.jsonnet
     ```
     Or `pixi shell` first if you prefer an interactive session.
   - Benchmark targets driven by `just` live behind the `bench` feature
     (`pixi shell -e bench`); see `docs/benchmarks.md`.
   - Add new workflows or benchmarks if you introduce new features.
8. **Submission**: Open a Pull Request against the `main` branch. Ensure the CI passes.

## Coding Style

- **C++**: We use C++23. Style is defined in `.clang-format`.
- **Python**: Follow PEP 8 (enforced by `ruff`).
- **Configuration**: Workflows are defined using [Jsonnet](https://jsonnet.org/).

## Licensing

This project is licensed under the **LGPL-3.0-or-later**. All contributions must be compatible with this license. Each new file must include an SPDX identifier and copyright notice.
