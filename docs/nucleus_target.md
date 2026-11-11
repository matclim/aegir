# Proton–nucleus collisions (`fixed_target_source`, `nucleus: true`)

By default `fixed_target_source` follows FairShip's `FixedTargetGenerator`:
each proton on target is one proton–proton or proton–neutron collision, the
target nucleon chosen with probability Z/A, and everything the nucleus does
beyond that first collision is left to Geant4. With `nucleus: true` the first
interaction is instead a full proton–nucleus collision from Pythia's heavy-ion
model, Angantyr: several nucleon–nucleon sub-collisions per event (on average
about 2–4 for Mo or W at 400 GeV), hadronised together.

```jsonnet
sources: {
  pythia8: lib.fixed_target {
    target_z: 42,
    target_a: 96,
    nucleus: true,   // default false: p-p / p-n as FairShip
  },
},
```

## How it is set up

- Beams: `2212` on the nucleus `100ZZZAAA0` built from `target_z`, `target_a`,
  `HeavyIon:mode = 1`. Pythia's particle table has only a few nuclei
  (²⁰⁸Pb, ¹⁹⁷Au, ⁶³Cu, …); any other target (e.g. ⁹⁶Mo, ¹⁸⁴W) is added as
  `ZzzAaa` with charge 3Z and mass A u (u = 0.931494 GeV), as Pythia's own
  nuclei. Without it Pythia's event check rejects every event.
- Frame: the event is generated in the nucleon–nucleon centre-of-mass frame
  (`Beams:frameType = 1`, `Beams:eCM` = √s<sub>NN</sub>, 27.43 GeV at
  400 GeV) and boosted to the lab (target at rest) with
  β = p<sub>beam</sub> / (E<sub>beam</sub> + m<sub>N</sub>); production
  vertices are boosted with the momenta. Pythia's fixed-target frame
  (`frameType = 2`, `eB = 0`) does not work with Angantyr: its nucleon–nucleon
  set-up gets no valid beam momentum for the target, and generation fails
  ("too many attempts to generate a working impact parameter point").
- Processes: `SoftQCD:inelastic` only. The photon-collision, prompt-photon
  and electroweak processes of the p–p/p–n set-up are not enabled.
- Output: the target's nuclear remnants (status 1, |PDG| ≥ 10⁹, at rest) are
  dropped, as Geant4 has no particle for them by PDG code.
- `pythia_settings` apply as usual (after aegir's own settings), so Angantyr's
  options (`Angantyr:*`, `HeavyIon:*`) can be changed from the configuration.

## Effect on the first interaction

p + ⁹⁶Mo at 400 GeV (Pythia 8.311, 2000 events), charged pions and kaons per
inelastic collision in the lab, against the p–p/p–n mix (Z/A = 42/96):

| E > [GeV] | π± p–p/p–n | π± p–Mo | K± p–p/p–n | K± p–Mo |
|---|---|---|---|---|
| 5 | 4.05 | 5.97 | 0.50 | 0.77 |
| 10 | 2.79 | 3.76 | 0.37 | 0.52 |
| 20 | 1.66 | 2.00 | 0.23 | 0.31 |
| 50 | 0.60 | 0.53 | 0.080 | 0.075 |
| 100 | 0.17 | 0.11 | 0.024 | 0.017 |

The extra sub-collisions add soft and medium-energy hadrons and take energy
from the leading ones.

## Random numbers and reproducibility

The main Pythia instance draws from aegir's Philox engine, re-keyed to the
event number like the p–p/p–n instances. Angantyr's internal generators
(minimum-bias and diffractive sub-collisions, hadronisation) use Pythia's own
engine, seeded with `Random:seed` from the job seed. The source is serial, so a
job is reproducible for a given seed, but unlike the default mode an event is
not keyed to its event number alone.

## Cost

Angantyr fits its sub-collision model to the nucleon–nucleon cross sections
when it initialises (about a minute) and prints the fitted parameters; adding
them as `HeavyIon:SigFitNGen = 0` and `HeavyIon:SigFitDefPar = ...` to
`pythia_settings` skips the fit for later jobs at the same energy.
Generation itself is slower than a single p–N collision, but Geant4 tracking
still dominates the time per event.
