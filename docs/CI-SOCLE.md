# CI socle — self-hosted runners on R630

> This page preserves the CI description that used to live in `README.md`
> before Cadence's project documentation took over the front page.
> It applies to every repository of the organisation.

## What this repository starts with

- **N self-hosted workers** on the R630 server (Debian 13, 80 cores):
  `r630-<repo>-01` …, labels `self-hosted, <repo>-worker, ci-node, Linux, X64`,
  systemd services `actions.runner.<owner>-<repo>.r630-<repo>-0N.service`.
- **hermes-lite gate** (`.github/workflows/hermes-lite.yml` +
  `.github/hermes/gate.py`) running on the workers: check `hermes-lite/verdict`,
  labels `hermes/approuvee` / `hermes/conflit`. In **dry-run** at launch
  (repository variable `HERMES_LITE_DRY_RUN=1`): it evaluates and labels, it
  does not merge.
- **Mergify** (`.mergify.yml`): merge queue, rebase, batching 1–3, labels
  `urgent` / `priority-low`, automatic boarding via
  `merge_protections_settings.auto_merge_conditions: true`. One-time setup:
  GitHub **All repositories** + enable the **Merge Queue** product in the
  dashboard.
- **stale-base-guard**: informational check on branch lag.
- **ci-budget** (`.github/workflows/ci-budget.yml` +
  `.github/ci-budget/check_timeouts.py`): guard for the 5-minute CI budget
  (user rule, 2026-10-24). Every job must declare `timeout-minutes: 5` or
  less; a longer job means CI is refused (the `ci-budget` check is required by
  `queue_conditions` in `.mergify.yml`). Any new CI follows: `concurrency` +
  `cancel-in-progress`, `fetch-depth: 1`, `paths:` filters, cache. Native build
  or long suite: outside the PR (tag, `main`, or `workflow_dispatch`), with
  the timeout justified in a comment.
  Detail: `~/Bureau/CI-BUDGET-5MIN.md`.

## Bringing it into service (order)

1. (done by the agent) workers registered and online;
2. **Mergify** app installed in "All repositories", **Merge Queue** product
   enabled in the dashboard (installing the app ≠ enabling the product);
3. `HERMES_LITE_DRY_RUN`: keep it while Mergify remains the only merge driver
   (the `MERGE_QUEUE_ENABLED` contract of 2026-10-10); remove it when the gate
   becomes the one and only merge driver — **never two drivers**.

## Pages publication

`site/` is published on GitHub Pages by `.github/workflows/pages.yml` — same
conventions as the other repositories of the organisation: `timeout-minutes: 5`,
`concurrency` with `cancel-in-progress`, checkout with `fetch-depth: 1`, and a
`paths:` filter so a documentation-only commit does not redeploy the site.
