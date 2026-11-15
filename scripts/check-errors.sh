#!/usr/bin/env bash
# Error policy of src/ (needs no PostgreSQL and no build), see CONTRIBUTING.md "Errors say what to do":
#   - an ERROR with SQLSTATE 54000 (limit), 0A000 (unsupported) or 55000 (wrong state) names what to do: errhint or errdetail;
#   - no ERROR has SQLSTATE XX000: a condition the code rules out is SIFT_INVARIANT(cond, "text") (core/host.h), anything else
#     a reachable condition with its own errcode (54000, 0A000, 55000, 40001, XX001/XX002 for corruption, ...).
#   - a corruption error (ERRCODE_INDEX_CORRUPTED, ERRCODE_DATA_CORRUPTED) whose message does not name the index (the SIFT_CORRUPT macros do; a decoder gets bytes and
#     cannot) is raised under a context scope of the index (core/errctx.h): its file is listed in scripts/errctx.tsv with the files whose entry points push the
#     scope, each of which must call sift_errctx_push. A file with such a message that is not listed is an error; "-" lists a known gap.
#   - no message, detail, hint or context names a relation by its OID (`index %u`): a name, or sift_rel_label(oid) when it may be gone (core/sift.h);
#   - a hint that writes DDL for a relation (`ALTER INDEX %s`, `REINDEX INDEX %s`, `DROP INDEX %s`) passes the name through sift_col_index_ref(), sift_index_ident() or quote_identifier(): a name that
#     needs quotes pastes as written.
#   - an option error says which value it refused: `invalid NAME option "VALUE": why` (sift_col_option_error), never `invalid NAME option: why`.
#   An `errcode(ERRCODE_INTERNAL_ERROR)` is XX000 too. The test-only files (*_test.c, test.c, and the files of a directory test/, compiled only with SIFT_TESTING) are
#   not checked; any other site may say `check-errors: allow` in a comment on the line of the call or one of the two above it.
#
#   scripts/check-errors.sh
set -euo pipefail

SRC=$(cd "$(dirname "$0")/../src" && pwd)
exec python3 -I - "$SRC" "$(dirname "$0")/errctx.tsv" <<'PY'
import os, re, sys

src = sys.argv[1]
registry = sys.argv[2]
NEEDS_HELP = {"ERRCODE_PROGRAM_LIMIT_EXCEEDED", "ERRCODE_FEATURE_NOT_SUPPORTED", "ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE"}
call = re.compile(r"\b(ereport|elog)\s*\(")
detail_call = re.compile(r"\b(errmsg|errmsg_plural|errdetail|errhint|errcontext)\s*\(")
oid_name = re.compile(r'"[^"]*\b(?:relation|index|table)\s+%u')
ddl_hint = re.compile(r'"[^"]*\b(?:ALTER|REINDEX|DROP)\s+(?:INDEX|TABLE)\b[^"]*%s')
option_no_value = re.compile(r'"invalid \w+ option:')
quoted = re.compile(r"\b(?:sift_col_index_ref|sift_index_ident|quote_identifier|quote_qualified_identifier)\s*\(")


def blank(text):
    """Comments and the inside of string and character literals become spaces (newlines stay); the comments are returned by line."""
    out, notes, i, n, line = [], {}, 0, len(text), 1
    while i < n:
        c = text[i]
        if text.startswith("/*", i) or text.startswith("//", i):
            j = text.find("*/", i + 2) + 2 if text[i + 1] == "*" else text.find("\n", i)
            j = n if j < 2 else j
            body = text[i:j]
            for k, part in enumerate(body.split("\n")):
                notes[line + k] = notes.get(line + k, "") + part
            out.append(re.sub(r"[^\n]", " ", body))
            line += body.count("\n")
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(c + " " * (j - i - 1) + c)
            line += text[i:j + 1].count("\n")
            i = j + 1
        else:
            out.append(c)
            line += c == "\n"
            i += 1
    return "".join(out), notes


def balanced(text, start):
    depth = 0
    for j in range(start, len(text)):
        depth += text[j] == "("
        depth -= text[j] == ")"
        if depth == 0:
            return j + 1
    return len(text)


bad = []
files = 0
unnamed = set()		# files with a corruption message that names no index
for root, _, names in os.walk(src):
    for name in sorted(names):
        if not name.endswith((".c", ".h")):
            continue
        path = os.path.join(root, name)
        with open(path, encoding="utf-8") as f:
            raw = f.read()
        if (re.search(r"(^|_)test(_am)?\.c$", name) or os.path.basename(root) == "test") and "SIFT_TESTING" in raw:
            continue
        text, notes = blank(raw)
        files += 1
        for m in detail_call.finditer(text):
            end = balanced(text, m.end() - 1)
            body = raw[m.end():end - 1]
            n = text.count("\n", 0, m.start()) + 1
            if any("check-errors: allow" in notes.get(k, "") for k in (n - 2, n - 1, n)):
                continue
            where = f"{os.path.relpath(path, os.path.dirname(src))}:{n}"
            if oid_name.search(body):
                bad.append(f"{where}: a relation named by its OID (%u): use its name, or sift_rel_label(oid)")
            if m.group(1) == "errmsg" and option_no_value.search(body):
                bad.append(f"{where}: an option error without the value of the option: use sift_col_option_error(option, value, why)")
            if m.group(1) == "errhint" and ddl_hint.search(body) and not quoted.search(body):
                bad.append(f"{where}: a hint with DDL for a relation: pass the name through sift_col_index_ref() or quote_identifier()")
        for m in call.finditer(text):
            end = balanced(text, m.end() - 1)
            body = text[m.end():end - 1]
            level = re.match(r"\s*(\w+)", body)
            if not level or level.group(1) not in ("ERROR", "FATAL"):
                continue
            n = text.count("\n", 0, m.start()) + 1
            if any("check-errors: allow" in notes.get(k, "") for k in (n - 2, n - 1, n)):
                continue
            where = f"{os.path.relpath(path, os.path.dirname(src))}:{n}"
            # the raw text keeps the names of the errcode and report calls (blank() kept every identifier)
            codes = re.findall(r"\berrcode\s*\(\s*(\w+)", body)
            if codes and codes[0] in ("ERRCODE_INDEX_CORRUPTED", "ERRCODE_DATA_CORRUPTED") and "RelationGetRelationName" not in body:
                unnamed.add(os.path.relpath(path, os.path.dirname(src)))
            if m.group(1) == "elog" or not codes or codes[0] == "ERRCODE_INTERNAL_ERROR":
                bad.append(f"{where}: XX000 ({m.group(1)}): use SIFT_INVARIANT(cond, \"text\") for a condition the code rules out, or an errcode")
            elif codes[0] in NEEDS_HELP and not re.search(r"\berr(hint|detail)\w*\s*\(", body):
                bad.append(f"{where}: {codes[0]} without errhint or errdetail: say what to do")

# the registry: FILE <TAB> files that push the context scope its errors are raised under (or "-": a known gap)
scopes, gaps = {}, 0
with open(registry, encoding="utf-8") as f:
    for line in f:
        line = line.split("#")[0].strip()
        if line:
            name, _, rest = line.partition("\t")
            scopes[name.strip()] = rest.split()
for name in sorted(unnamed - scopes.keys()):
    bad.append(f"{name}: a corruption message that names no index, in a file scripts/errctx.tsv does not list (the context of core/errctx.h names it: list the file with the scope files that push it)")
for name in sorted(scopes.keys() - unnamed):
    bad.append(f"scripts/errctx.tsv: {name} has no corruption message that names no index (or is not a file of src/): remove the line")
for name, pushers in sorted(scopes.items()):
    gaps += pushers == ["-"]
    for scope in pushers:
        if scope == "-":
            continue
        path = os.path.join(os.path.dirname(src), scope)
        if not os.path.isfile(path) or "sift_errctx_push" not in open(path, encoding="utf-8").read():
            bad.append(f"scripts/errctx.tsv: {name}: {scope} does not push a context scope (sift_errctx_push)")

for b in bad:
    print(b)
if bad:
    print(f"check-errors: {len(bad)} site(s)", file=sys.stderr)
    sys.exit(1)
print(f"check-errors: ok, {files} files, {len(unnamed)} with a corruption message that names no index, {gaps} of them without a context scope")
PY
