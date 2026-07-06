# Neutrino flux ntuple — schema proposal

Status: **draft for discussion with the neutrino group** (schema version 1).

## Why

For the planned GENIE library integration (see `docs/genie.md`), the flux
enters GENIE through a flux driver that hands over one neutrino ray at a
time. The current FairShip flux files
(`pythia8_Geant4_*_nu.root`) only carry histograms — the momentum spectrum
and a (log p, log pT) distribution per flavour — so energy, direction and
production point are decorrelated, and the neutrino direction has to be
re-drawn from the 2D histogram (the source of the pT truncation artefact,
FairShip issue #984). The upstream simulation knows the full kinematics of
every neutrino; this schema keeps them.

The information is already present in the production files: FairShip's
`muonShieldOptimization/extractNeutrinosAndUpdateWeight.py` reads exactly
these quantities (`MCTrack` kinematics, mother track, per-sample POT weight)
and fills histograms with them. The converter to this schema reads the same
inputs and writes entries instead.

## Contents

One ROOT file with two RNTuples:

### `nu_flux` — one entry per neutrino

| field | type | unit | meaning |
|:---|:---|:---|:---|
| `pdg` | int32 | — | neutrino PDG code (±12, ±14, ±16) |
| `vx`, `vy`, `vz` | float64 | mm | production (decay) vertex, SHiP global frame |
| `t` | float64 | ns | production time relative to the primary proton interaction (0 if not simulated) |
| `px`, `py`, `pz` | float64 | GeV | neutrino momentum |
| `weight` | float64 | — | statistical weight, see normalisation below |
| `parent_pdg` | int32 | — | decaying parent (π, K, μ, τ, charm/beauty hadron) |
| `parent_px`, `parent_py`, `parent_pz` | float64 | GeV | parent momentum at decay |
| `process_id` | int32 | — | production sample: 0 = min. bias, 1 = charm, 2 = beauty |
| `origin_run`, `origin_event` | int64 | — | run/event in the upstream production file, for tracing full ancestry |

The production vertex plus the momentum define the neutrino ray exactly —
no reference plane is needed; downstream consumers extrapolate through
whatever geometry they use. Units and frame follow the SHiP conventions
used by `SHiP::MCParticle` (mm, ns, GeV).

`parent_pdg`/`parent_*` cost little and enable flavour-composition and
decay-kinematics studies without going back to the production files;
anything deeper (grandparents, full decay chains) is reachable via
`origin_run`/`origin_event`.

### `flux_meta` — one entry per file

| field | type | meaning |
|:---|:---|:---|
| `schema_version` | int32 | 1 |
| `pot` | float64 | protons-on-target equivalent of this file |
| `max_energy` | float64 | highest neutrino energy in the file (GeV) — lets readers size spline ranges without a full scan |
| `description` | string | free text: sample, geometry tag, production campaign |
| `software` | string | producing software and version |

## Normalisation

The contract: **the expected number of neutrinos produced per `pot`
protons on target is the sum of `weight` over all entries.** For a plain
sample `weight` is 1.0 per entry. Samples with enhanced statistics
(charm/beauty productions with POT-equivalents different from the
minimum-bias sample) are merged by rescaling their weights to the merged
file's single `pot` value: `weight *= pot / pot_sample`. This reproduces
the relative weights FairShip applies today (e.g. min. bias 768.75 vs
charm 326 for 5×10¹³ POT), but keeps them per entry instead of baked into
histogram fills.

This gives the GENIE driver what it needs for proper exposure accounting:
after generating events for the whole file (or a weighted fraction), the
delivered protons on target are known exactly, replacing the fixed-σ yield
arithmetic criticised in FairShip issue #984.

## Mapping onto the GENIE flux driver

Each `nu_flux` entry is one flux ray: `PdgCode()` = `pdg`, `Position()` =
vertex (converted mm → m, ns → s: GENIE uses SI), `Momentum()` from
`px,py,pz`, `Weight()` = `weight`. `MaxEnergy()` comes from `flux_meta`.
The driver can run weighted (hand GENIE the weight) or unweighted
(accept–reject on `weight` first); the schema supports both.

## Converting existing productions

`scripts/convert_fairship_nu_flux.py` converts FairShip `cbmsim` files to
this format. It reads the trees leaf-by-leaf, so it needs no FairShip
libraries — any environment with ROOT ≥ 6.40 works:

```sh
# one sample -> one flux file (weights from MCTrack.fW)
convert_fairship_nu_flux.py convert -o mbias_nu.root --pot 1.8e9 \
    --process mbias pythia8_Geant4_1.0_c*.root

# combine samples with different POT equivalents at a common reference
convert_fairship_nu_flux.py merge -o flux.root --pot 5e13 \
    mbias_nu.root charm_nu.root
```

By default all neutrino tracks are converted; `--selection
points:vetoPoint` restricts to neutrinos with a scoring-plane crossing,
matching the 2018 histogram extraction. Charm/beauty/tau-descended
neutrinos are excluded from min. bias samples (they come from the
dedicated productions) unless `--keep-charm` is given.

## Open points for the neutrino group

- Field list: is parent-at-decay enough, or should polarisation
  (relevant for ν from μ decay) be stored? dk2nu keeps the full decay
  record for this reason; a `parent_polarisation` triple would be the
  minimal addition.
- Time origin convention (proton-on-target vs. spill clock).
- Whether converters should also emit the legacy histograms for
  backwards compatibility with the FairShip pipeline.
- Storage: RNTuple assumed (ROOT ≥ 6.40 everywhere in the SHiP stack);
  TTree fallback only if a consumer requires it.
