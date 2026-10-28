#!/usr/bin/env bash
# A pg_sift.so built for AArch64 must run on any ARMv8-A CPU (Graviton 2/3/4, Ampere, Azure/GCP Arm, Raspberry Pi, Apple silicon VMs):
# the code uses NEON (Advanced SIMD) and nothing newer. Disassembles each file and fails on an instruction of an optional or later extension:
# LSE atomics, CRC32, dot product, FP16 arithmetic, SVE/SME, non-hint pointer authentication, and the crypto, RCpc, complex-number,
# BF16/I8MM, FlagM, MTE and rounding extensions. The libgcc outline-atomics helpers (__aarch64_*) hold LSE instructions behind a runtime
# check of the kernel's hardware capabilities, so they are allowed (by name, or by their shape in a stripped file: ldrb w16 of the flag, cbz w16, the instruction); PAC/BTI hint encodings (paciasp, autiasp, bti) execute as no-ops on older CPUs.
# The source has no compile-time __ARM_FEATURE_* test and no runtime detection beyond NEON, so a hit means the toolchain targeted a newer
# architecture (-march/-mcpu, a distribution default); the flags come from pg_config's CFLAGS.
#
#   scripts/check-aarch64-baseline.sh [pg_sift.so ...]    needs an objdump that decodes AArch64 (GNU objdump of an arm64 host, or llvm-objdump)
set -euo pipefail
export LC_ALL=C

objdump=
for t in "${OBJDUMP:-}" objdump llvm-objdump; do
	[[ -n $t ]] && command -v "$t" >/dev/null && [[ $("$t" -i 2>/dev/null) == *aarch64* || $("$t" --version 2>&1) == *LLVM* ]] && { objdump=$t; break; }
done
[[ -n $objdump ]] || { echo "check-aarch64-baseline: no objdump that decodes AArch64 (install llvm or binutils-multiarch, or set OBJDUMP)" >&2; exit 1; }
(($#)) || set -- pg_sift.so

re_lse='^((ld|st)(add|clr|eor|set|smax|smin|umax|umin)p?(a|l|al)?[bh]?|swpp?(a|l|al)?[bh]?|casp?(a|l|al)?[bh]?|ldlar[bh]?|stllr[bh]?|sqrdml[as]h)$'
re_isa='^(crc32c?[bhwx]|[su]dot|usdot|sudot|[su]smmla|usmmla|ummla|bfdot|bfmmla|bfmlal[bt]|bfcvtn2?|bfcvt|fmlal2?|fmlsl2?|fcadd|fcmla|fjcvtzs|frint(32|64)[xz]|rmif|setf(8|16)|cfinv|axflag|xaflag|sb|ldapr[bh]?|ldapur(s?[bhw])?|stlur[bhw]?)$'
re_pac='^((pac|aut)[id]z?[ab]|pacga|xpac[id]|(ret|bra|brb|blra|blrb|eret|ldr)a[ab]z?)$'
re_mte='^(irg|gmi|addg|subg|ldgm?|stgm|stgp|st2g|stzg|stz2g|stzgm)$'
re_crypto='^(aes[ed]|aes(i)?mc|sha1(c|p|m|h|su0|su1)|sha256(h2?|su0|su1)|sha512(h2?|su0|su1)|eor3|rax1|xar|bcax|sm3.*|sm4.*)$'
re_sve='^(rdvl|addvl|addpl|cnt[bhwd]|ptrue|ptrues|pfalse|whilel[eost]|whilerw|whilewr|rdffr|wrffr|setffr|smstart|smstop|(sq|uq)?(inc|dec)[bhwdp])$'

rc=0
for so in "$@"; do
	[[ -f $so ]] || { echo "check-aarch64-baseline: $so: no such file" >&2; exit 1; }
	machine=$(readelf -h "$so" | sed -n 's/^ *Machine: *//p')
	[[ $machine == AArch64 ]] || { echo "check-aarch64-baseline: $so: ELF machine is '$machine', not AArch64"; rc=1; continue; }
	# the x86 SIMD kernels are compiled on x86-64 only; a symbol of one here means the wrong objects were linked
	if readelf -sW "$so" | grep -qiE 'avx(2|512)|_pdep|_pext'; then echo "check-aarch64-baseline: $so: x86 kernel symbols"; rc=1; fi
	out=$("$objdump" -d --no-show-raw-insn "$so" | awk -F'\t' -v lse="$re_lse" -v isa="$re_isa" -v pac="$re_pac" -v mte="$re_mte" -v crypto="$re_crypto" -v sve="$re_sve" '
		/^[0-9a-f]+ <.*>:$/ { fn = $0; sub(/^[0-9a-f]+ </, "", fn); sub(/>:$/, "", fn); next }
		NF >= 2 && $1 ~ /^ *[0-9a-f]+:/ {
			mn = $2; sub(/ .*/, "", mn); ops = $3
			why = ""
			if (mn ~ lse) why = "LSE atomic"
			else if (mn ~ isa) why = "ARMv8.1+ extension"
			else if (mn ~ pac) why = "non-hint pointer authentication"
			else if (mn ~ mte) why = "MTE"
			else if (mn ~ crypto || (mn ~ /^pmull2?$/ && ops ~ /\.1q/)) why = "crypto extension"
			else if (mn ~ sve || ops ~ /(^|[ ,{[])z[0-9]+(\.|[,}\]]|$)/ || ops ~ /(^|[ ,{[])p[0-9]+\/[mz]/) why = "SVE/SME"
			else if (mn ~ /^(f|scvtf|ucvtf)/ && mn !~ /^fcvt(l2?|n2?)?$/ && ops ~ /(^|[ ,{[])h[0-9]+|\.[248]h/) why = "FP16 arithmetic"
			if (why == "") { p2 = p1; p1 = mn " " ops; next }
			if (why == "LSE atomic" && (fn ~ /^__aarch64_/ || (p1 ~ /^cbz w16,/ && p2 ~ /^ldrb w16,/))) { k++; p2 = p1; p1 = mn " " ops; next }
			if (!(fn SUBSEP mn in seen)) { seen[fn SUBSEP mn] = 1; hits[++n] = sprintf("  %s: %s (%s)", fn, mn, why) }
			p2 = p1; p1 = mn " " ops
		}
		END { for (i = 1; i <= n; i++) print hits[i]; if (k) print "outline " k }')
	outline=$(grep -c '^outline ' <<<"$out" || true)
	hits=$(grep -v '^outline ' <<<"$out" || true)
	if [[ -n $hits ]]; then
		echo "check-aarch64-baseline: $so uses instructions above ARMv8-A:"
		echo "$hits" | head -40
		rc=1
	else
		n=0; ((outline)) && n=$(sed -n 's/^outline //p' <<<"$out")
		echo "check-aarch64-baseline: $so ok, only ARMv8-A and NEON instructions ($n libgcc outline-atomics helpers, each behind a runtime check)"
	fi
done
exit $rc
