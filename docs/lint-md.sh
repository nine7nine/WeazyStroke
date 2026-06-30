#!/usr/bin/env bash

set -euo pipefail

warn_count=0
err_count=0

warn() {
    printf 'lint-md: warning: %s\n' "$*" >&2
    warn_count=$((warn_count + 1))
}

error() {
    printf 'lint-md: error: %s\n' "$*" >&2
    err_count=$((err_count + 1))
}

check_top_structure() {
    local file="$1"
    local head40 head80 intro_block intro_paras intro_sentences

    head40=$(sed -n '1,40p' "$file")
    head80=$(sed -n '1,80p' "$file")
    if ! grep -q '^## Table of Contents' "$file"; then
        return
    fi

    intro_block=$(awk '
        NR==1 { next }
        /^## Table of Contents/ { exit }
        /^---$/ { next }
        { print }
    ' "$file")

    if grep -Eq '^(Status:|\*\*Status:\*\*|> \*\*Status:|\*\*Date:\*\*|Author:)' <<<"$head40"; then
        error "$file: page top still contains metadata/status banner content"
    fi

    if grep -Eq 'Scope of this page' <<<"$head80"; then
        error "$file: page top still contains a scope section"
    fi

    intro_paras=$(perl -0ne '
        s/^\s+|\s+$//g;
        if (length($_)) {
            @p = grep { /\S/ } split(/\n\s*\n/, $_);
            print scalar(@p);
        } else {
            print 0;
        }
    ' <<<"$intro_block")

    if [[ "${intro_paras:-0}" -gt 1 ]]; then
        error "$file: page top should be a single short intro paragraph before the TOC"
    fi

    intro_sentences=$(perl -0ne '
        s/\n/ /g;
        s/\s+/ /g;
        my $count = () = /[.!?](?:["'"'"'`)]+)?(?:\s|$)/g;
        print $count;
    ' <<<"$intro_block")

    if [[ "${intro_sentences:-0}" -gt 2 ]]; then
        warn "$file: page top intro should stay to one or two sentences before the TOC"
    fi
}

check_svg_source_rules() {
    local file="$1"
    local content

    content=$(cat "$file")

    if grep -Eq 'marker id=|marker-end=|marker-start=|marker-mid=' <<<"$content"; then
        error "$file: source SVG still uses arrow markers; author plain connectors instead"
    fi

    if grep -Eq '#3b4261[^[:cntrl:]]*stroke-dasharray|stroke-dasharray[^[:cntrl:]]*#3b4261' <<<"$content"; then
        warn "$file: source SVG still uses the older dark dashed guide color; prefer #6b7398"
    fi
}

files=("$@")
if [[ ${#files[@]} -eq 0 ]]; then
    files=(*.md)
fi

for file in "${files[@]}"; do
    [[ -f "$file" ]] || continue
    check_top_structure "$file"
    check_svg_source_rules "$file"
done

if (( err_count > 0 )); then
    printf 'lint-md: %d error(s), %d warning(s)\n' "$err_count" "$warn_count" >&2
    exit 1
fi

if (( warn_count > 0 )); then
    printf 'lint-md: %d warning(s)\n' "$warn_count" >&2
fi
