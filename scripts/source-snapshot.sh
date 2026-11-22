# shellcheck shell=bash
# Copy current tracked and nonignored untracked sources without Git history.
# The caller owns the destination and its lifecycle.
copy_git_sources() {
	( cd "$1" && git ls-files -z --cached --others --exclude-standard | tar --null -T - -cf - ) | tar -C "$2" -xf -
}
# drop_build DIR: the objects and the module a private copy of the tree built, once its library is installed under the job's root: 110 MB of the scratch for each flavor
# (hardened, the release library) that nothing reads again
drop_build() {
	find "$1" -type f \( -name '*.o' -o -name '*.d' \) -not -path '*/.git/*' -delete
	find "$1" -maxdepth 1 -type f \( -name '*.so' -o -name '*.dylib' \) -delete
}
