# AGENTS.md

## Project Type

This repository is an ESP-IDF project.

## ESP-IDF Environment

The ESP-IDF installation path is configured in the repository-local `.env` file.

Before running any ESP-IDF related command, read `.env` and use `ESP_IDF_PATH` from it.

Expected `.env` format:

```
ESP_IDF_PATH=/path/to/esp-idf
```

Because Codex shell commands may run in fresh non-interactive shells, always activate ESP-IDF in the same command that runs idf.py.

Use this pattern:

```
bash -lc 'set -a && source .env && set +a && source "$ESP_IDF_PATH/export.sh" >/dev/null && idf.py build'
```

For other ESP-IDF commands, use the same pattern:

```
bash -lc 'set -a && source .env && set +a && source "$ESP_IDF_PATH/export.sh" >/dev/null && idf.py menuconfig'
bash -lc 'set -a && source .env && set +a && source "$ESP_IDF_PATH/export.sh" >/dev/null && idf.py set-target esp32c6'
bash -lc 'set -a && source .env && set +a && source "$ESP_IDF_PATH/export.sh" >/dev/null && idf.py build'
```