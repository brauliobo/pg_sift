"""What the generators of scripts/ (gen-options.sh, gen-limits.sh, gen-reference.sh) share: reading C source and writing markdown cells.
They import it with `sys.path.insert(0, 'scripts')` from the repository root (they run as `python3 -I -`, which has no script directory)."""
import functools
import re


@functools.lru_cache(maxsize=None)   # the selftests of the generators parse the same sources many times
def strip_comments(text):
    """the text with every comment replaced by blanks (same offsets and lines); strings and character constants stay"""
    def blank(m):
        t = m.group(0)
        return t if t[0] in '"\'' else re.sub(r'[^\n]', ' ', t)
    return re.sub(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|/\*.*?\*/|//[^\n]*', blank, text, flags=re.S)


def split_args(s):
    """the top level arguments of a C call's argument text: commas inside parentheses, braces and strings do not split"""
    out, depth, cur, i = [], 0, '', 0
    while i < len(s):
        c = s[i]
        if c == '"':
            j = i + 1
            while s[j] != '"':
                j += 2 if s[j] == '\\' else 1
            cur += s[i:j + 1]
            i = j + 1
            continue
        depth += (c in '({') - (c in ')}')
        if c == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += c
        i += 1
    return out + [cur.strip()]


def norm(s):
    return re.sub(r'\s+', ' ', s).strip()


def esc(s):
    return s.replace('|', '\\|')
