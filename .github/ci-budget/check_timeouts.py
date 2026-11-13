#!/usr/bin/env python3
"""Garde-fou « budget CI » : tout job GitHub Actions doit être borné à 5 min.

Règle user 2026-10-24. Bibliothèque standard uniquement (les runners
self-hosted n'ont pas PyYAML garanti). Analyse volontairement simple :
on lit les workflows ligne à ligne, on repère la section `jobs:` et, pour
chaque job (clé à 2 espaces), le `timeout-minutes:` à 4 espaces.

Erreurs (exit 1) : job sans `timeout-minutes`, ou timeout > MAX_MINUTES.
Avertissements (exit 0) : `concurrency` absent, checkout sans fetch-depth: 1.

Exception : un workflow qui ne tourne JAMAIS sur une pull request et qui
porte un job long explicitement justifie est exempt (regle "sortir des PR"
de ~/Bureau/CI-BUDGET-5MIN.md). `qemu-e2e.yml` est le banc de conformite
long du depot passerelle : il ne se declenche que sur `workflow_dispatch` ou
sur un push sur `main`, avec un `timeout-minutes` de 30 min commente dans
le YAML. Sans cette entree, le garde-fou bloquerait toutes les PR.
"""

import glob
import os
import re
import sys

MAX_MINUTES = 5
WORKFLOWS = ".github/workflows"
SKIP_FILES = {"qemu-e2e.yml"}  # job long hors PR (justifie dans le YAML)

TOP_KEY = re.compile(r"^[A-Za-z_][A-Za-z0-9_-]*:")
JOB_KEY = re.compile(r"^  ([A-Za-z0-9_.-]+):\s*(#.*)?$")
TIMEOUT = re.compile(r"^    timeout-minutes:\s*(\d+)\s*(#.*)?$")


def read_lines(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read().splitlines()


def scan_jobs(lines):
    """Retourne [(nom, timeout|None)] pour la section jobs: du fichier."""
    jobs = []
    in_jobs = False
    for line in lines:
        if line.startswith("jobs:"):
            in_jobs = True
            continue
        if in_jobs and TOP_KEY.match(line):
            in_jobs = False
        if not in_jobs:
            continue
        match = JOB_KEY.match(line)
        if match:
            jobs.append([match.group(1), None])
            continue
        match = TIMEOUT.match(line)
        if match and jobs:
            jobs[-1][1] = int(match.group(1))
    return jobs


def check_file(path, errors, warnings):
    rel = path
    lines = read_lines(path)
    jobs = scan_jobs(lines)
    if not jobs:
        warnings.append("%s: aucun job détecté (workflow vide ?)" % rel)
        return
    for name, timeout in jobs:
        if timeout is None:
            errors.append(
                "%s: job '%s' sans timeout-minutes (défaut 360 min, interdit)"
                % (rel, name))
        elif timeout > MAX_MINUTES:
            errors.append(
                "%s: job '%s' timeout-minutes: %d > %d (budget CI)"
                % (rel, name, timeout, MAX_MINUTES))
    if not any(line.startswith("concurrency:") for line in lines):
        warnings.append("%s: pas de concurrency (annulation des runs obsolètes)"
                        % rel)
    if "actions/checkout" in "\n".join(lines) and "fetch-depth: 1" not in "\n".join(lines):
        warnings.append("%s: checkout sans fetch-depth: 1" % rel)


def main():
    root = os.environ.get("CI_BUDGET_ROOT", ".")
    paths = sorted(glob.glob(os.path.join(root, WORKFLOWS, "*.yml")))
    paths += sorted(glob.glob(os.path.join(root, WORKFLOWS, "*.yaml")))
    paths = [p for p in paths if os.path.basename(p) not in SKIP_FILES]

    errors, warnings = [], []
    if not paths:
        print("::warning::Aucun workflow dans %s" % WORKFLOWS)
    for path in paths:
        check_file(os.path.relpath(path, root), errors, warnings)

    for message in warnings:
        print("::warning::%s" % message)
    for message in errors:
        print("::error::%s" % message)

    print("%d workflow(s) contrôlé(s) : %d erreur(s), %d avertissement(s)"
          % (len(paths), len(errors), len(warnings)))
    if errors:
        print("Règle : CI ≤ %d min par job — voir ~/Bureau/CI-BUDGET-5MIN.md"
              % MAX_MINUTES)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())