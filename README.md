# Cadence

Cadence is an ambient voice assistant. This repository holds its **QNX Neutrino
port**: the llama.cpp porting layer, the QNX build files, the image and boot
scripts, and the documentation.

## QNX porting layer

> **Submission note** — this is a *port*, not a fork. llama.cpp is never
> vendored here: the upstream project is referenced by URL and pinned to an
> exact commit, and the port is delivered as a patch. That is what the QNX
> Global Hackathon rules ask for.

| Item | Value |
| --- | --- |
| Upstream | <https://github.com/ggml-org/llama.cpp> |
| Pinned commit | `9daa0aa22b8aa0143344d7549dee1f0e32f048bb` |
| Target | QNX Neutrino 8.0, `x86_64`, SDP 8.0 |
| Measured in the guest | 5.93 t/s (pp128) and 4.00 t/s (tg64) with Ternary-Bonsai 2B Q2_0 |

| Path | Content |
| --- | --- |
| `patches/0001-qnx-port.patch` | The port: 5 files, +53/-1 lines. |
| `patches/README.md` | What each modification fixes and why. |
| `patches/qnx.nto.toolchain.cmake` | CMake toolchain for the QNX SDP. |
| `tools/build-llama.sh` | Clone at the pinned commit, apply the patch, build. |
| `tools/fetch-model.sh` | Download the GGUF model and check its SHA-256. |
| `images/buildfile` | The IFS buildfile (boot chain, drivers, guest scripts). |
| `images/build-image.sh` | `mkifs` + `mkqnx6fsimg`: build the image and the model disk. |
| `images/make-model-disk.sh` | Build the qnx6 filesystem that carries the model. |
| `images/run-qemu.sh` | Boot the IFS under QEMU (KVM and TCG). |
| `images/bench.sh` | The in-guest benchmark. |
| `docs/BUILD.md` | Build and run instructions, and the list of QNX gotchas. |
| `NOTICE` | Licences and third-party attributions. |

```sh
make check    # the patch applies on a pristine upstream checkout
make llama    # build llama.cpp for QNX
make model    # download + checksum the model
make ifs      # build the IFS and the model disk
make bench    # boot under KVM and run the in-guest benchmark
```

`make help` lists every target. The build targets require the QNX SDP 8.0 and
fail with an explicit message when it is missing.

---

# Socle CI self-hosted (R630)

## Ce dépôt démarre avec

- **N workers** self-hosted sur le serveur R630 (Debian 13, 80 cœurs) :
  `r630-<repo>-01` …, labels `self-hosted, <repo>-worker, ci-node, Linux, X64`,
  services systemd `actions.runner.<owner>-<repo>.r630-<repo>-0N.service`.
- **Gate hermes-lite** (`.github/workflows/hermes-lite.yml` + `.github/hermes/gate.py`)
  tournant sur les workers : check `hermes-lite/verdict`, labels `hermes/approuvee` /
  `hermes/conflit`. En **dry-run** au lancement (variable de dépôt
  `HERMES_LITE_DRY_RUN=1`) : il évalue et étiquette, il ne merge pas.
- **Mergify** (`.mergify.yml`) : file de merge, rebase, batching 1-3, labels
  `urgent` / `priority-low`, auto-embarquement par
  `merge_protections_settings.auto_merge_conditions: true`. À faire une fois :
  app GitHub **All repositories** + activation du produit **Merge Queue** dans
  le dashboard Mergify.
- **stale-base-guard** : check informatif de retard de branche.
- **ci-budget** (`.github/workflows/ci-budget.yml` +
  `.github/ci-budget/check_timeouts.py`) : garde-fou du budget CI de
  **5 minutes** (règle user 2026-10-24). Chaque job doit déclarer
  `timeout-minutes: 5` au plus ; un job plus long = CI refusée (le check
  `ci-budget` est exigé par `queue_conditions` dans `.mergify.yml`). Toute
  nouvelle CI suit : `concurrency` + `cancel-in-progress`, `fetch-depth: 1`,
  filtres `paths:`, cache. Build natif / suite longue : hors PR (tag,
  `main`, ou `workflow_dispatch`), timeout justifié en commentaire.
  Détail : `~/Bureau/CI-BUDGET-5MIN.md`.

## Mise en service (ordre)

1. (fait par l'agent) workers enregistrés et en ligne ;
2. app GitHub **Mergify** installée en « All repositories », produit Merge Queue
   activé sur le dashboard (app installée ≠ produit activé) ;
3. `HERMES_LITE_DRY_RUN` : la garder si Mergify reste le seul pilote de merge
   (contrat duello `MERGIFY_QUEUE_ENABLED` 2026-10-10), la retirer pour que le
   gate merge lui-même — **jamais deux pilotes**.