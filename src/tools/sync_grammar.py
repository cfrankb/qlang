#!/usr/bin/env python3
import json
import re
import os

# Project workspace path anchors
SPEC_PATH = "../../SPEC.md"
GRAMMAR_PATH = "../../editors/vscode/syntaxes/qlang.tmLanguage.json"

def extract_list_from_markdown(file_path, start_marker, end_marker):
    """Extracts words contained between custom HTML comment blocks inside markdown files."""
    if not os.path.exists(file_path):
        return []

    with open(file_path, "r", encoding="utf-8") as f:
        content = f.read()

    # Locate block slices using comment regex
    pattern = f"{start_marker}\\s*```(?:python)?\\s*(.*?)\\s*```\\s*{end_marker}"
    match = re.search(pattern, content, re.DOTALL)
    if not match:
        return []

    # Filter out empty items, code markers, or comment lines
    raw_lines = match.group(1).splitlines()
    words = []
    for line in raw_lines:
        clean = line.strip().split("#")[0].strip() # strip comments
        if clean and not clean.startswith("=>"):
            # If line has multiple options/aliases like 'str (alias string)', extract word boundaries
            tokens = re.findall(r"\b[a-zA-Z_][a-zA-Z0-9_]*\b", clean)
            words.extend(tokens)
    return sorted(list(set(words)))

def update_vscode_grammar():
    print("--> Synchronizing language definitions from SPEC.md...")

    # 1. Parse current specification items
    keywords = extract_list_from_markdown(SPEC_PATH, "<!-- START_KEYWORDS -->", "<!-- END_KEYWORDS -->")
    types = extract_list_from_markdown(SPEC_PATH, "<!-- START_TYPES -->", "<!-- END_TYPES -->")

    if not keywords or not types:
        print("Error: Could not extract keywords or types from SPEC.md. Check markers.")
        return

    # 2. Open and update TextMate JSON data dictionary
    if os.path.exists(GRAMMAR_PATH):
        with open(GRAMMAR_PATH, "r", encoding="utf-8") as f:
            grammar = json.load(f)

        # Compile lists into safe alternated regex pipe segments strings
        keyword_regex = f"\\\\b({'|'.join(keywords)})\\\\b"
        type_regex = f"\\\\b({'|'.join(types)})\\\\b"

        # Update specific JSON node array patterns matching names
        for pattern in grammar.get("patterns", []):
            if pattern.get("name") == "keyword.control.qlang":
                pattern["match"] = keyword_regex
            elif pattern.get("name") == "storage.type.qlang":
                pattern["match"] = type_regex

        # 3. Flush completely updated JSON content structure back out to file
        with open(GRAMMAR_PATH, "w", encoding="utf-8") as f:
            json.dump(grammar, f, indent=2)

        print(f"Success: Updated grammar with {len(keywords)} keywords and {len(types)} datatypes.")
    else:
        print(f"Error: Target file not found at {GRAMMAR_PATH}")

if __name__ == "__main__":
    update_vscode_grammar()
