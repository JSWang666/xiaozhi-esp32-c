# Board build matrix (CI)

This repository builds **every board variant** discovered under `main/boards/**/config.json` when you push to `main` (see `.github/workflows/build.yml`).

## How variants are enumerated

`scripts/release.py` walks `main/boards`, reads each `config.json`, and emits one matrix entry per `builds[]` item:

```bash
python scripts/release.py --list-boards --json
```

The GitHub Actions **prepare** job runs that command and feeds the JSON into the **build** matrix (`fail-fast: false` so one failing board does not cancel the rest).

## Local full matrix

Same as CI (slow — each variant runs `idf.py set-target` + full build):

```bash
source $IDF_PATH/export.sh
python scripts/release.py all
```

Compile-only (no `merge-bin`, no `releases/*.zip`, useful for smoke tests):

```bash
python scripts/release.py all --skip-package
```

Single variant:

```bash
python scripts/release.py waveshare/esp32-s3-lcd-0.85 --name esp32-s3-lcd-0.85 --skip-package
```

## Scheduled compile-only job

Pushes to `main` still produce `merged-binary.bin` artifacts per variant.

A **weekly** `schedule` run executes the same matrix with `--skip-package` to validate that all targets still compile without uploading multi-hundred-megabyte artifacts to GitHub.

## Manual dispatch

Use **Actions → Build Boards → Run workflow**. Optional input **skip package** mirrors `--skip-package` for a lighter run.

## ESP-IDF container version

The workflow pins `espressif/idf` to the image tag declared in `.github/workflows/build.yml` (currently aligned with the upstream project default). Bump it when upgrading ESP-IDF for the whole tree.
