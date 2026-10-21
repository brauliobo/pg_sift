#!/usr/bin/env python3
"""Writes src/agg/vk_min.h: the part of the Vulkan API that src/agg/vk.c uses, copied verbatim from the Khronos header.

A VULKAN=1 build then needs no Vulkan SDK or headers (libvulkan.so.1 is opened with dlopen at run time). The declarations are the
interface of the loader, which does not change: a Vulkan function or structure keeps its layout in every later version.

    scripts/gen-aggvk-min.py [--header /usr/include/vulkan/vulkan_core.h]        regenerate the file
    scripts/gen-aggvk-min.py --check                                              fail when the file differs from what the header gives

The names are those agg/vk.c mentions (identifiers starting with Vk, PFN_vk or VK_), and what their declarations need.
Vulkan headers: Apache-2.0 OR MIT, Copyright 2015-2025 The Khronos Group Inc.
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src/agg/vk.c")
OUT = os.path.join(ROOT, "src/agg/vk_min.h")

ap = argparse.ArgumentParser()
ap.add_argument("--header", default="/usr/include/vulkan/vulkan_core.h")
ap.add_argument("--check", action="store_true")
args = ap.parse_args()

core = open(args.header).read()
use = open(SRC).read()
use = use[use.index("#else\t") if "#else\t" in use else use.index("#include <ctype.h>"):]
use = re.sub(r'"[^"\n]*"', '""', use)       # string literals name nothing
version = re.search(r"#define VK_HEADER_VERSION (\d+)", core).group(1)

# ---- index of the declarations of the header ----
structs = {m.group(1): m.group(0) for m in re.finditer(r"typedef struct (\w+) \{.*?\n\} \1;", core, re.S)}
enums = {m.group(1): m for m in re.finditer(r"typedef enum (\w+) \{(.*?)\n\} \1;", core, re.S)}
handles = {m.group(2): m.group(0) for m in re.finditer(r"VK_DEFINE(_NON_DISPATCHABLE)?_HANDLE\((\w+)\)", core)}
tdefs = {m.group(2): m.group(0) for m in re.finditer(r"^typedef ((?:uint\d+_t|VkFlags\d*|Vk\w+)) (\w+);$", core, re.M)}
pfns = {m.group(1): m.group(0) for m in re.finditer(r"typedef [\w\s*]+?\(VKAPI_PTR \*(PFN_\w+)\)\(.*?\);", core, re.S)}
defines = {}
for m in re.finditer(r"^#define (VK_\w+)(?:\([\w, ]+\))? +(.*?)(?<!\\)$", core, re.M | re.S):
    if "\n" not in m.group(0) or m.group(0).count("\\\n") == m.group(0).count("\n"):
        defines.setdefault(m.group(1), m.group(0))
enumerator = {}
for name, m in enums.items():
    for e in re.finditer(r"^\s+(VK_\w+) = [^,\n]+,?", m.group(2), re.M):
        enumerator.setdefault(e.group(1), (name, e.group(0).strip().rstrip(",")))

wanted_enums = {}       # enum name -> set of enumerators
wanted = {"struct": [], "handle": [], "tdef": [], "pfn": [], "define": []}
seen = set()


def need(name):
    if name in seen:
        return
    seen.add(name)
    if name in structs:
        for dep in re.findall(r"\b(Vk\w+|PFN_vk\w+|VK_\w+)\b", structs[name].split("{", 1)[1]):
            need(dep)
        wanted["struct"].append(name)
    elif name in handles:
        wanted["handle"].append(name)
    elif name in tdefs:
        need(re.match(r"typedef (\w+)", tdefs[name]).group(1))
        wanted["tdef"].append(name)
    elif name in pfns:
        for dep in re.findall(r"\b(Vk\w+|PFN_vk\w+|VK_\w+)\b", pfns[name].replace("typedef", "", 1)):
            need(dep)
        wanted["pfn"].append(name)
    elif name in enumerator:
        en = enumerator[name][0]
        wanted_enums.setdefault(en, set()).add(name)
        val = enumerator[name][1].split("=", 1)[1].strip()
        for dep in re.findall(r"\bVK_\w+\b", val):
            need(dep)
        need(en)
    elif name in enums:
        wanted_enums.setdefault(name, set())
        # the bits enum of a Flags typedef
    elif name in defines:
        for dep in re.findall(r"\bVK_\w+\b", defines[name].split(None, 2)[2] if len(defines[name].split(None, 2)) > 2 else ""):
            if dep != name:
                need(dep)
        wanted["define"].append(name)
    # anything else (a C type, a macro of vk_platform.h) is not ours


for n in sorted(set(re.findall(r"\b(?:Vk\w+|PFN_vk\w+|VK_\w+)\b", use))):
    if n.startswith("VK_INSTANCE_FNS") or n.startswith("VK_DEVICE_FNS") or n in ("VK_NULL_HANDLE",):
        continue
    need(n)
# the function pointer types of the X-macro lists
for n in re.findall(r"\bX\((vk\w+)\)", use):
    need("PFN_" + n)
for n in ("PFN_vkGetInstanceProcAddr", "PFN_vkCreateInstance"):
    need(n)
# flag typedefs name their bits enum: VkFooFlags <- VkFooFlagBits
for t in list(wanted["tdef"]):
    bits = t.replace("Flags", "FlagBits")
    if bits in enums and bits != t:
        wanted_enums.setdefault(bits, set())

out = []
out.append("""/*
 * agg/vk_min.h - the Vulkan declarations agg/vk.c uses, copied from the Khronos header vulkan_core.h (header version %s) by
 * scripts/gen-aggvk-min.py; do not edit. A VULKAN=1 build needs no Vulkan SDK: the library is opened with dlopen and the functions are
 * taken from it by name. Only the structures, enumerators and function pointer types the backend mentions are here.
 * The Vulkan headers are Apache-2.0 OR MIT, Copyright 2015-2025 The Khronos Group Inc.
 */
#ifndef SIFT_AGGVK_MIN_H
#define SIFT_AGGVK_MIN_H

#include <stddef.h>
#include <stdint.h>

#define VKAPI_ATTR
#define VKAPI_CALL
#define VKAPI_PTR
#define VK_NO_PROTOTYPES

#define VK_DEFINE_HANDLE(object) typedef struct object##_T *object;
#if defined(__LP64__) || defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__) || defined(__powerpc64__)
#define VK_DEFINE_NON_DISPATCHABLE_HANDLE(object) typedef struct object##_T *object;
#define VK_NULL_HANDLE ((void *) 0)
#else
#define VK_DEFINE_NON_DISPATCHABLE_HANDLE(object) typedef uint64_t object;
#define VK_NULL_HANDLE 0ULL
#endif
""" % version)
for d in wanted["define"]:
    if d in ("VK_NULL_HANDLE",):
        continue
    out.append(defines[d].rstrip())
out.append("")
for h in wanted["handle"]:
    out.append(handles[h])
out.append("")
for t in wanted["tdef"]:
    out.append(tdefs[t])
out.append("")
for name in sorted(wanted_enums):
    body = enums[name].group(2)
    lines = []
    for line in body.split("\n"):
        em = re.match(r"\s+(VK_\w+) =", line)
        if em and (em.group(1) in wanted_enums[name] or em.group(1).endswith("_MAX_ENUM")):
            lines.append(line.rstrip())
    out.append("typedef enum %s {\n%s\n} %s;" % (name, "\n".join(lines), name))
out.append("")
# a function pointer type that names no structure goes first (a structure may hold one); the others take structures
plain = [p for p in wanted["pfn"] if not any(re.search(r"\b%s\b" % s, pfns[p]) for s in wanted["struct"])]
out.extend(pfns[p] for p in plain)
out.append("")
for s in wanted["struct"]:
    out.append(structs[s])
out.append("")
for p in wanted["pfn"]:
    if p not in plain:
        out.append(pfns[p])
out.append("\n#endif\t\t\t\t\t\t\t/* SIFT_AGGVK_MIN_H */\n")
text = "\n".join(out)
text = re.sub(r"\n{3,}", "\n\n", text)

if args.check:
    cur = open(OUT).read() if os.path.exists(OUT) else ""
    if cur != text:
        sys.stderr.write("src/agg/vk_min.h differs from what scripts/gen-aggvk-min.py gives\n")
        sys.exit(1)
    print("gen-aggvk-min: ok")
else:
    open(OUT, "w").write(text)
    print("wrote %s (%d lines, header version %s)" % (OUT, text.count("\n"), version))
