#!/usr/bin/env bash
set -e

QLANG="cfrankb.qlang-0.1.0"
TARGET_DIR="$HOME/.vscode/extensions"

# Triage 1: Auto-detect sandboxed environments (Snap / Flatpak / Insiders)
if [ -d "$HOME/snap/code/current" ]; then
    TARGET_DIR="$HOME/snap/code/current/.vscode/extensions"
elif [ -d "$HOME/.var/app/com.visualstudio.code/data/vscode/extensions" ]; then
    TARGET_DIR="$HOME/.var/app/com.visualstudio.code/data/vscode/extensions"
elif [ -d "$HOME/.vscode-insiders/extensions" ]; then
    TARGET_DIR="$HOME/.vscode-insiders/extensions"
fi

echo "--> Target Extension Path detected: $TARGET_DIR"

# Triage 2: Rebuild pristine deployment directory tree
mkdir -p "$TARGET_DIR/$QLANG/syntaxes"

# Triage 3: Sync configuration files safely
cp qlang.tmLanguage.json "$TARGET_DIR/$QLANG/syntaxes/qlang.tmLanguage.json"
cp package.json           "$TARGET_DIR/$QLANG/package.json"
cp language-configuration.json "$TARGET_DIR/$QLANG/language-configuration.json"

echo "--> Configuration files synchronized successfully."

# Triage 4: Clear workspace storage table cache records
echo "--> Flushing VSCode optimization caches..."
rm -rf ~/.config/Code/CachedData/* 2>/dev/null || true
rm -rf ~/.config/Code/User/workspaceStorage/* 2>/dev/null || true

echo "--> Deployment complete! Please restart VSCode completely."
