#!/bin/bash
# Compares mariadb/host/host_regex.c with the regex engine of PostgreSQL 18: every (pattern, word) pair is answered by the real
# pg_regcomp/pg_regexec (flags and calls of src/bm25/query_analyze.c and query_match.c, through the throwaway extension tests/rx_oracle.c in a
# private cluster under /tmp/sift-mdb-rx) and by regex_test.c linked with host_regex.c; the answers (1, 0 or error) must
# be identical, for REG_ADVANCED|REG_NOSUB (what the engine uses) and for REG_ADVANCED alone. The prefixes
# pg_regprefix returns must be true prefixes of every word the pattern matches.
#   mariadb/tests/regex_check.sh
set -eu

HERE=$(cd "$(dirname "$0")/.." && pwd)
W=/tmp/sift-mdb-rx
PORT=56310
PG_INC=$(pg_config --includedir-server)
PSQL="env -u PGUSER -u PGHOST -u PGPORT psql -h $W -p $PORT -d postgres -XAtq -v ON_ERROR_STOP=1"

cleanup() {
	[ -d "$W/pg" ] && pg_ctl -D "$W/pg" -m immediate stop >/dev/null 2>&1 || true
	rm -rf "$W"
}
cleanup
trap cleanup EXIT
mkdir -p "$W"

cat >"$W/words.txt" <<'EOF'

a
b
aa
ab
ba
abc
abcd
abab
aaa
aaaa
abba
abcabc
bab
a b
a-b
a_b
a.b
a/b
a\b
a*b
ab12
12
007
a1
A
AB
Abc
ABC
aBc
xyz
foo
foobar
foo_bar
foo-bar
hello
hello world
world
under_score
0x1F
deadbeef
a1b2c3
Zz
cafe
café
naïve
résumé
ÉCOLE
école
日本語
日本
日
こんにちは
αβγ
ΑΒΓ
привет
ПРИВЕТ
😀
a😀b

  a
mississippi
abcabcabc
racecar
level
xyxy
hello-hello
AbAb
aAbB
12341234
a1a1
abcba
aabb
EOF
{
	printf 'a\001b\n\001\nab\001\na\001b\001c\n'
	printf 'a%.0s' $(seq 200); echo
	printf 'ab%.0s' $(seq 100); echo
	printf '日%.0s' $(seq 60); echo
} >>"$W/words.txt"

# one pattern per line, as SiftQL gives it between the slashes
cat >"$W/pats.txt" <<'EOF'
a
abc
a.c
a..
...
.
.*
.+
.?
a\/b
a\.b
a\\b
a\-b
a\*b
a\*
\.
\x61bc
\x61\x62
\U00000061bc
\x65e5
\n
\t
\x41
\101
\0
\cA
\e
\a\b
a\sb
a\Sb
\d
\d+
\D+
\w+
\W
\W+
\s
\S+
[0-9]+
[a-z]+
[a-z]*
[^a-z]+
[^0-9]
[A-Z][a-z]+
[abc]
[^abc]*
[a-c]{2,3}
[[:alpha:]]+
[[:digit:]]+
[[:alnum:]]+
[[:upper:]]
[[:lower:]]+
[[:space:]]
[[:punct:]]
[[:xdigit:]]+
[[:alpha:][:digit:]]+
[[:word:]]+
[[:blank:]]
[[:print:]]+
[[:graph:]]+
[[:cntrl:]]
[[:ascii:]]+
[\d]+
[\w]+
[\s]
[\D]+
[\W]
[\S]+
[\w\s]+
[^\w]
[\x61-\x63]+
[\n]
[\t ]
[\]]
[\-a]
[a\-c]
[]a]
[^]a]
[a-]
[-a]
[a-c-e]
[a--]
[+--]
[z-a]
[[.a.]]
[[=a=]]b
[[.-.]]
[a-[.c.]]
[[:foo:]]
[[:alpha:]-z]
[\d-z]
[a-\d]
[\1]
[\m]
[\B]
[a
[[:alpha:]
[
^a
a$
^a$
^
$
a|b
a|b|c
ab|cd
(a|b)c
(?:a|b)c
a(b|c)d
(ab)+
(?:ab)+
(?:ab)*
(?:ab)?
(?:ab){2}
(?:ab){1,2}
(ab){2,}
a*
a+
a?
a*?
a+?
a??
a{2}
a{2,}
a{2,3}
a{0}
a{0,1}
a{1,2}?
a{2}?
a{2,}?
a{,2}
a{
a{x}
a{ 2}
a}
}
{
]
a]
a{1,2}b
(a*)*
(a*)+
(a+)+
(a|b)*
(a|ab)*c
(a|)+
(|a)+
()
(|)
|
|a
a|
a||b
(a)(b)(c)
((a)(b))
((a)|b)*
x*
.*b
.*a.*b.*
a*a*a*a*b
(a+)+b
(a|aa)+$
(.*)*b
(.*)*
a{200}
a{199,}
.{3}
.{2}
.{1,3}
.{0,}
a(?=b)
a(?=b).
a(?!b)
a(?!b).
(?=a)a
(?!a).*
(?!ab).*
(?=.*b).*
(?=.*b)a.*
(?!.*b).*
(?=.{3}$).*
(?=\d)\w+
(?!\d)\w+
a(?=b)b
(?:(?=a))?aa
(?!)
(?=)a
(?<=a)b
a(?<=a)b
a(?<!b)b
(?<=^)a
.(?<=a)
..(?<=ab)
(?<!^).
(?<=a|aa)b
(?<=\w)\w
(?<=(a))b
a(?<=a)
\mfoo
foo\M
\yfoo\y
\w+\y
\w+\M
\m\w+
\Yo
f\Yo
\yhello\y.*
hello\y.world
hello\M.\mworld
\Bx
[[:<:]]foo
foo[[:>:]]
[[:<:]]foo[[:>:]]
\A
\Aab
ab\Z
\A.*\Z
a\Zb
\y
\Y
\m
\M
\m*
(a)\1
(a)\1+
(a+)\1
(.)\1
(.)(.)\2\1
(.)(.)\1\2
(.+)\1
(.*)\1
(ab)\1
(\w+)-\1
(\w+)\1
(a|b)\1
(a|b)+\1
(a*)\1
(a)?\1
(a)|b\1
\1
(a)\2
(a\1)
(?:(a)|b)\1
(a)(?:b|\1)
((a)\2)+
(a)b\1
(.)\1\1
(.)(?=\1)
(a)\10
(a)\011
(?i)A.c
(?i)ABC
(?i)abc
(?i)É
(?i)école
(?i)[a-c]+
(?i)[A-C]+
(?i)[^a]+
(?i)[[:upper:]]+
(?i)[[:lower:]]+
(?i)\x41bc
(?i)(a)\1
(?i)(.)\1
(?c)abc
(?ci)ABC
(?ic)ABC
(?n)a.b
(?n)^b
(?n)a$
(?n)^a$
(?n).*
(?n)[^x]+
(?n)\W+
(?n)\Aa
(?n)a\Z
(?p)a.b
(?p)^b$
(?w)a.b
(?w)^b
(?w)a$
(?s)a.b
(?t)abc
(?m)a$
(?ins)ABC
(?in)a.b
(?i)
(?x)a b
(?q)a
(?z)abc
(?e)ab
a(?i)b
(?i
(?)
(?i)(?c)a
a(?#comment)b
a(?#x)*
(?#)a
(?#)
a(?#
a(
a)
(a
)
())
(?:
(?=
(?<
(?<a)b
(?q
*a
+a
?a
a**
a+*
a?*
a*+
a{1}{2}
a**b
^*
$*
(?=a)*
\y*
(*a)
(+)
(?:?)
a|*
a{2,1}
a{256}
a{1
a{1,2
a{2,x}
a{2 }
a?{2}
a\
\
\q
\8
\9
\xzz
\x
\u12
\U0062
\c
\E
\x7fffffff
\x110000
日.語
日本.
.本.
[日本]+
[^日]+
[あ-ん]+
é
caf.
caf[eé]
na.ve
[à-ÿ]+
😀
a.😀
\w+
\W
(\w)(\w)\2\1
(\w)\w?\1
(.)(.).?\2\1
(.+)\1+
((.)(.))\1
(.)(.)\2
(a|b)*c\1
(?:(a)|(b))\2
(?:a|(b))\1
(a)?(?:\1)
(?:(.)\1)+
(.)\1(?!\1).*
(?=(.)\1).*
([ab]+)\1
((a|b)\2)+
(?i)(a|b)\1
(?i)([a-z]+)-\1
(?:(?=(a))a)\1
(a){0}b
(a){0}\1
(?:){2}
(?:)*
(?:)+b
(a{2}){2}
(a{1,2}){2,3}
(?:a{0,2}b?){3}
(a|ab|abc){2}
(a+?)(a*)
(a+?)(a+?)
(a*?)(a*)\2
a^b
a$b
^a^b
(?<!\w)\w+(?!\w)
(?<=\d)(?=\D)
(?<=\d)\D
\w(?<=\y.)
(?<=\m\w)\w*
(?<=a)(?<=b)
(?<=ab)c
(?<=a+)b
(?<=a{2})b
(?<![a-c])\w+
(?<=^|-)\w+
\w+(?=-|$)
(?=(\w)).\1
(?:a|b)*?b
(?:a|b)+?
(?:a*?)*b
(?:a?)*
(?:a?)+?b
(?:a|)*b
(?:a*|b)*
(a*)*(b*)*
(a|b)*?(b)
x*y*z*
(x*)*
((x*)*)*
(?:x*)*y
(?:x*y?)*
(?:(?:a|b)*c)*
(?:\w+\s?)+
(?:[a-z]+[0-9]*)+
([a-z]+)([0-9]+)?
^(?:a|b)+$
(?:^a|b$)+
(?:a|^b)+
(?:^|-)\w+
(?:\W|^)\w+(?:\W|$)
[[:alpha:]]+(?:-[[:alpha:]]+)*
[[:alpha:]]{3,}
[^[:alpha:]]
[^[:alpha:][:digit:]]+
[[:^alpha:]]
[a-cx-z]+
[a-c-]+
[^-a]+
[*+?{}()|]+
[\\]
[\^a]
[a^]
[[]
[\[]a
[a-a]
[--/]
[.]
(?i)[.]
(?i)[^.]
(?i)[é]
(?i)日
(?i)[[:alpha:]]+
(?i)\w+
(?i)\W
(?i)[\W]
(?i)\x61\x62\x63
(?i)a|B
(?i)(?:A|b)+
(?i)A{2}
(?i)\1
(?i)(a)(?-i)\1
(?ci)a
(?ic)A
(?cic)A
(?n)a|b
(?n)[a\n]+
(?n)a\nb
(?n)^.*$
(?n)^[^\n]*$
(?w)^.*$
(?p)^.*$
(?p)\n
(?n)(?:^|\n)b
(?n)\y
a{1,2}{
x{2}{3
x{2,3}?
x{2,}?y
x{0}
x{0,0}?
x{1}?
(x{0})*
(x{0,1})*y
\x{2}
\x61{2}
[\x61]{2}
\061
\0141
\7
(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)\10
(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)\11
\00
\18
\1a
\ 
\@
\_
\é
\ñ
\"
\'
\/
\#
\&
\~
\<
\>
\=
\!
\,
\:
\;
\%
EOF
# \u escapes, spelled so that this file holds no complete \uHHHH sequence
printf '\\u%s\n' 0061bc 65e5 '65e5.*' '00e9' '0062' '006' >>"$W/pats.txt"

# a fixed-seed fuzz of atoms and quantifiers: many are invalid, which is also checked
awk 'BEGIN {
	srand(42)
	na = split("a b c . [ab] [^a] [a-c] \\w \\d \\W x (a) (b) (a|b) (?:a|b) (?:ab) (a*) (?=a) (?!b) (?<=a) (?<!b) \\1 \\2 ^ $ \\y \\Y \\m \\M ab a|b", atoms, " ")
	nq = split("* + ? {2} {1,2} {0,2} *? +? ?? {2,} {2}?", quants, " ")
	for (i = 0; i < 900; i++) {
		p = rand() < 0.1 ? "(?i)" : ""
		for (j = 1 + int(rand() * 5); j > 0; j--) {
			p = p atoms[1 + int(rand() * na)]
			if (rand() < 0.35) p = p quants[1 + int(rand() * nq)]
		}
		print p
	}
}' >>"$W/pats.txt"

awk -F'\n' 'NR == FNR {w[n++] = $0; next} {for (i = 0; i < n; i++) print $0 "\t" w[i]}' \
	"$W/words.txt" "$W/pats.txt" >"$W/pairs.tsv"

nice gcc -O2 -fPIC -shared -I"$PG_INC" -o "$W/rx_oracle.so" "$HERE/tests/rx_oracle.c"
nice gcc -O2 -g -std=gnu11 -fwrapv -fno-strict-aliasing -D_GNU_SOURCE -Wall -Wno-declaration-after-statement \
	-I"$HERE/host" -I"$HERE/../src" -o "$W/regex_test" "$HERE/host/host_regex.c" "$HERE/tests/regex_test.c" -lm

initdb -D "$W/pg" -A trust -E UTF8 --locale=C >/dev/null
pg_ctl -D "$W/pg" -o "-k $W -p $PORT -c listen_addresses=''" -l "$W/pg.log" -w start >/dev/null
$PSQL <<EOF
CREATE FUNCTION rx_match(text, text, int) RETURNS text AS '$W/rx_oracle.so' LANGUAGE C STRICT;
CREATE FUNCTION rx_prefix(text, int) RETURNS text AS '$W/rx_oracle.so' LANGUAGE C STRICT;
CREATE TABLE pairs (id serial, pat text, word text);
COPY pairs (pat, word) FROM '$W/pairs.tsv' WITH (FORMAT csv, DELIMITER E'\t', QUOTE E'\x02', NULL E'\x03');
EOF

total=0
equal=0
for mode in "nosub 19" "sub 3"; do
	set -- $mode
	$PSQL -c "COPY (SELECT rx_match(pat, word, $2) FROM pairs ORDER BY id) TO '$W/pg.$1'"
	"$W/regex_test" match "$1" <"$W/pairs.tsv" >"$W/my.$1"
	paste "$W/pairs.tsv" "$W/pg.$1" "$W/my.$1" | awk -F'\t' '$3 != $4 && !seen[$1]++' | head -40 | sed "s/^/MISMATCH ($1): /"
	n=$(wc -l <"$W/pairs.tsv")
	e=$(paste "$W/pg.$1" "$W/my.$1" | awk -F'\t' '$1 == $2' | wc -l)
	[ "$1" = nosub ] && echo "regex answers (nosub): $(sort "$W/pg.$1" | uniq -c | awk '{printf "%s x %s  ", $1, $2}')"
	total=$((total + n))
	equal=$((equal + e))
done

"$W/regex_test" prefix "$W/words.txt" <"$W/pats.txt" >"$W/pfx.my"
[ "$(paste "$W/pats.txt" "$W/pfx.my" | awk -F'\t' '$1 == "abc" {print $2}' | sort -u)" = "E:abc" ] || { echo "abc is not REG_EXACT"; exit 1; }
echo "regex prefixes: $(grep -c '^[PE]:' "$W/pfx.my") of $(wc -l <"$W/pats.txt") patterns, all true prefixes of their matches"

echo "regex checks: $equal/$total equal to PostgreSQL ($(wc -l <"$W/pats.txt") patterns, $(wc -l <"$W/words.txt") words)"
[ "$equal" -eq "$total" ]
