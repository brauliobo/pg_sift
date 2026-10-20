# Copy current tracked and nonignored untracked sources without Git history.
# The caller owns the destination and its lifecycle.
copy_git_sources() {
	( cd "$1" && git ls-files -z --cached --others --exclude-standard | tar --null -T - -cf - ) | tar -C "$2" -xf -
}
