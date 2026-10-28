<!--
SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration

SPDX-License-Identifier: LGPL-3.0-or-later
-->

# Physics parameters

The Pythia8 sources and the Geant4 module take their physics parameters from
the job configuration, so a variation or a tune needs no code change. Every
parameter is checked: a setting the generator or Geant4 does not accept fails
the job at startup instead of leaving the default silently in place.

## Pythia8 (`pythia8_source`, `fixed_target_source`)

| Key | Type | Meaning |
|---|---|---|
| `pythia_settings` | list of strings | Pythia8 settings, `'Key = value'`, as in a `.cmnd` file |
| `pythia_cmnd` | string | a command file, one setting per line, read before `pythia_settings` |

They are applied after aegir's own settings (beams, processes, `Print:quiet`
and, in `fixed_target_source`, the long-lived particles made stable) and
before `init()`, so they can override all of them: processes, tunes, PDFs,
string fragmentation, multiparton interactions, particle data
(`'443:mayDecay = off'`), and so on. `fixed_target_source` applies them to
both its instances (p–p and p–n).

- A line Pythia8 rejects (unknown key, bad value) is an error.
- Lines that do not start with a letter or digit are comments, as in Pythia8.
- `Random:` settings are refused: aegir seeds and keys Pythia8's random numbers
  itself, so use `seed`.
- After `init()` the serial sources log Pythia8's list of settings that differ
  from the defaults, so the log shows what was actually used.

## Geant4 (`geant4_module`)

| Key | Type | Meaning |
|---|---|---|
| `hadronic_parameters` | object of numbers | `G4HadronicParameters`, by the names below |
| `hadronic_flags` | object of booleans | `G4HadronicParameters` switches, by the names below |
| `developer_parameters` | object of numbers | `G4HadronicDeveloperParameters`: the FTF model parameters (`FTF_PION_AVRG_PT2`, `FTF_BARYON_PROC0_A1`, ...), in the units Geant4 registers them in (see `G4FTFTunings.cc`) |
| `developer_flags` | object of booleans | their switches (`FTF_PION_DIFF_DISSO_PROJ`, ...) |
| `commands` | list of strings | Geant4 UI commands |

All are applied on the master, in the PreInit state, which is where Geant4
accepts them:
1. the parameters and flags before the physics list is created, since its
   constructors read the model transition energies;
2. the commands after the physics list is created (its messengers, e.g. the EM
   extra physics', exist from then on) and before the run manager initialises.

The workers build their physics from the same objects afterwards.

### `hadronic_parameters` (energies in GeV)

| Name | `G4HadronicParameters` |
|---|---|
| `max_energy` | `SetMaxEnergy` |
| `min_energy_transition_ftf_cascade`, `max_energy_transition_ftf_cascade` | FTF ↔ Bertini/Binary transition |
| `min_energy_transition_qgs_ftf`, `max_energy_transition_qgs_ftf` | QGS ↔ FTF transition |
| `energy_threshold_for_heavy_hadrons` | `SetEnergyThresholdForHeavyHadrons` |
| `xs_factor_nucleon_inelastic`, `xs_factor_nucleon_elastic` | cross-section factors for p, n |
| `xs_factor_pion_inelastic`, `xs_factor_pion_elastic` | for π± |
| `xs_factor_hadron_inelastic`, `xs_factor_hadron_elastic` | for other hadrons and ions |
| `xs_factor_em` | for γ and leptons |

The cross-section factors act only with `hadronic_flags: { apply_factor_xs:
true }` (setting one without it is an error), and Geant4 limits them to
|factor − 1| < 0.2. A transition's maximum must lie above its minimum.

### `hadronic_flags`

`apply_factor_xs`, `enable_bc_particles`, `enable_hyper_nuclei`,
`enable_cr_coalescence`, `enable_integral_inelastic_xs`,
`enable_integral_elastic_xs`, `enable_diff_dissociation_for_b_greater_10`,
`enable_coherent_charge_exchange`.

### `commands`, examples

| Command | Effect |
|---|---|
| `/physics_lists/em/GammaToMuons true` | γ → μ⁺μ⁻ (Bethe–Heitler pairs) |
| `/physics_lists/em/GammaToMuonsFactor 2` | its cross section × 2 |
| `/physics_lists/em/PositronToMuons true` | e⁺e⁻ → μ⁺μ⁻ |
| `/physics_lists/em/MuonNuclear true` | μ–nucleus interactions |
| `/process/had/models/ftf/selectTuneByName pion-tune2022-v0` | an FTF tune (`default`, `baryon-tune2022-v0`, `pion-tune2022-v0`, `combined-tune2022-v0`, `energy-resolution-tune2023-v0`) |
| `/process/em/...`, `/process/msc/...`, `/process/eLoss/...` | `G4EmParameters` |

The EM option of the physics list is chosen by name: `physics_list:
'FTFP_BERT_EMZ'` (option 4), `'FTFP_BERT_EMY'` (option 3), etc.

### Errors

Geant4 ignores a value it does not accept (out of range, locked) without an
error, so every parameter is read back and a mismatch is an error, as is an
unknown name. A command fails the job unless it succeeds. The developer
parameters report an unknown name, a value out of range or a second change as
a fatal `G4Exception`, which would abort the process; while they and the
commands are applied, a handler turns fatal `G4Exception`s into errors, so the
job ends cleanly with Geant4's message.

## Parameter points and `run_info` (workflows/lib.libsonnet)

A parameter point, e.g. one point of a tune, is an object with a `pythia` and
a `geant4` part:

```jsonnet
{
  pythia: { pythia_settings: ['StringFlav:probStoUD = 0.22'] },
  geant4: {
    hadronic_flags: { apply_factor_xs: true },
    hadronic_parameters: { xs_factor_pion_inelastic: 1.05 },
    commands: ['/physics_lists/em/GammaToMuons true'],
  },
}
```

`lib.tune_part(point, 'pythia')` and `lib.tune_part(point, 'geant4')` return
the two parts, checked against `lib.tune_keys`, for merging into the blocks:

```jsonnet
local lib = import 'lib.libsonnet';
function(tune={}, seed=1)
  lib.with_run_info({
    ...
    sources: { pythia8: lib.fixed_target { seed: seed } + lib.tune_part(tune, 'pythia') },
    modules: {
      geant4: lib.geant4 { seed: seed } + lib.tune_part(tune, 'geant4'),
      output: lib.full_output('out.root', 'out_validation.root'),
    },
  })
```

with the point given as a file: `jsonnet --tla-code-file tune=point.json ...`.

`lib.with_run_info(workflow)` stores the whole job configuration, as JSON, in
the output file as the `TNamed` `run_info`, written once the RNTuple is
committed (`sim_output_module` key `run_info`). Every output then carries the
parameter point, the seeds and all other settings that made it:

```cpp
auto* info = file->Get<TNamed>("run_info");  // info->GetTitle(): the JSON
```

Set `seed` explicitly in such jobs: a seed drawn at startup is only logged,
not part of the configuration.
