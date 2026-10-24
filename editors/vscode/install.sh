set -e
QLANG=cfrankb.qlang-0.1.0
mkdir -p ~/.vscode/extensions/$QLANG/syntaxes
cp qlang.tmLanguage.json ~/.vscode/extensions/$QLANG/syntaxes/qlang.tmLanguage.json
cp package.json ~/.vscode/extensions/$QLANG/package.json
cp language-configuration.json ~/.vscode/extensions/$QLANG/language-configuration.json

python3 -c "import json, os; [json.load(open(os.path.expanduser(f))) for f in ['~/.vscode/extensions/$QLANG/package.json', '~/.vscode/extensions/$QLANG/language-configuration.json', '~/.vscode/extensions/$QLANG/syntaxes/qlang.tmLanguage.json']]"

# Wipe out the shared state and extension cache tables
rm -rf ~/.config/Code/CachedData/*
rm -rf ~/.config/Code/User/globalStorage/storage.json