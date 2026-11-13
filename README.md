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