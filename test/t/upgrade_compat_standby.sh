#!/usr/bin/env bash
# A physical standby and its primary on different libraries, the zoo of test/t/lib_compat.bash on both (every index kind; a release's WAL is PostgreSQL's own plus the custom
# resource manager records of a published sift_table, which a patch release does not change). Both directions, with the writes of every kind and the maintenance in between:
#   up:   the primary runs the release (SIFT_UPGRADE_OLD_REF, default v5.0.0), the standby this tree (the standby is updated first), then the primary is updated and restarted
#   down: the primary runs this tree, the standby the release (a rollback of the standby, or a standby that is not updated yet)
# Each stage: the standby verifies every object and answers as the heap, and as the primary does. The one exception is the down direction after the first writes of this tree: what 5.1
# writes that 5.0 cannot read (the deletion records of a tracked index: column format 52; the key-range pages of a key_cut write buffer: 1.7 with minor 55) must be refused by the older
# standby with the format error, not read as damage, and the tables of those indexes, which every plan opens, are refused with them; the rest of the zoo is read as before.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+' >&2; exit 1; }
. "$(dirname "$0")/lib.bash"
. "$(dirname "$0")/lib_compat.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)
if ((PG_MAJOR != 18)); then echo '1..0 # SKIP the released libraries build on PostgreSQL 18 only (two library roots)'; exit 0; fi
OLD_REF=${SIFT_UPGRADE_OLD_REF:-v5.0.0}
ROWS=$(knob SIFT_COMPAT_ROWS 6000 3000)
newroot=$ROOT
build_old "$OLD_REF"; oldroot=$OLDROOT

same() { # PRIMARY STANDBY: every object that the standby can read answers the same on both
	local o kind ix q p a b
	for o in "${OBJECTS[@]}"; do
		IFS='|' read -r kind ix q p <<<"$o"
		[[ " ${REFUSED_KINDS:-} " != *" $kind "* ]] || continue
		a=$(answer "$1" "${ON[@]}" "$q"); b=$(answer "$2" "${ON[@]}" "$q"); out="primary: $a / standby: $b"
		[[ $a == "$b" && $a != *ERROR* ]] || return 1
	done
}
no_damage() { # STANDBY: sift.check() says no index is damaged (it may refuse an index with the format error)
	out=$(problems "$1" "$SIFT_RELS"); [[ $out != *corrupted* && ( -z $out || $out =~ this\ build.*reads ) ]]
}
write_load() { # NODE SEED: writes of every kind
	sql "$1" \
		"INSERT INTO docs SELECT i, i % 9, i % 100, 'delta' || i % 3 FROM generate_series($2 + 1, $2 + 300) i" "UPDATE docs SET n = n + 2 WHERE id % 5 = 0" "DELETE FROM docs WHERE id % 17 = 0 AND id <= $2" \
		"INSERT INTO kc SELECT i, i % 20, i % 50 FROM generate_series($2 + 1, $2 + 300) i" "UPDATE kc SET v = v + 1 WHERE id % 6 = 0" \
		"INSERT INTO pt SELECT i, i % 9, i % 100 FROM generate_series($2 + 1, $2 + 300) i" "DELETE FROM pt WHERE id % 19 = 0 AND id <= $2" \
		"INSERT INTO events SELECT i, 1 + i % 420, now(), i % 10 FROM generate_series($2 + 1, $2 + 200) i" "DELETE FROM tenant_scope WHERE dim_a_id % 11 = 0" \
		"INSERT INTO fact SELECT i, 1 + i % 50, i % 30 FROM generate_series($2 + 1, $2 + 200) i" \
		"INSERT INTO trk_c SELECT i, i % 8, i % 60 FROM generate_series($2 + 1, $2 + 200) i" "UPDATE trk_c SET v = v + 1 WHERE id % 7 = 0" "DELETE FROM trk_c WHERE id % 23 = 0 AND id <= $2" \
		"INSERT INTO trk_i SELECT i, i % 8, i % 60 FROM generate_series($2 + 1, $2 + 200) i" "UPDATE trk_i SET v = v + 1 WHERE id % 7 = 0" "DELETE FROM trk_i WHERE id % 23 = 0 AND id <= $2" \
		"INSERT INTO tbl SELECT i, i % 9, i % 100 FROM generate_series($2 + 1, $2 + 300) i" "UPDATE tbl SET n = n + 1 WHERE id % 5 = 0" "DELETE FROM tbl WHERE id % 29 = 0 AND id <= $2" \
		"INSERT INTO vs SELECT i, md5(i::text) || md5('x' || i), 'b' || i % 13, i % 10 FROM generate_series($2 + 1, $2 + 200) i" "DELETE FROM vs WHERE id % 31 = 0 AND id <= $2" >/dev/null
}
maintain() { # NODE: VACUUM, the rewrite of the table, forced merges and col_maintain
	sql "$1" "VACUUM (ANALYZE) docs, kc, pt, events, fact, dim, trk_c, trk_i, vs" "SELECT sift.table_maintain('tbl')" "VACUUM FULL tbl" \
		"SELECT sift.merge(u::regclass, true) FROM unnest(ARRAY[$COLIDX, 'docs_idx']) u" "SELECT sift.col_maintain(u::regclass) FROM unnest(ARRAY[$COLIDX]) u" >/dev/null
}
stage() { # LABEL PRIMARY STANDBY
	check "$1: the standby replays" catchup "$2" "$3"
	compat "$1" standby "$3"
	check "$1: the standby answers as the primary" same "$2" "$3"
	if [[ -z ${REFUSED_KINDS:-} ]]; then check "$1: sift.check() of the standby is empty" sound_all "$3"; else check "$1: sift.check() of the standby refuses what it cannot read" no_damage "$3"; fi
}

run() { # DIRECTION PRIMARY_ROOT STANDBY_ROOT OFFSET
	local dir=$1 proot=$2 sroot=$3 p=cp_$1 s=cs_$1 off=$4
	REFUSED_KINDS=
	node_init "$p" "$off" "$CONF
max_wal_senders = 4"
	use_root "$p" "$proot"
	node_start "$p" || bail "$dir: primary start failed"
	zoo_create "$p" || bail "$dir: setup failed"
	sql "$p" "VACUUM (ANALYZE) docs, kc, pt, events, fact, dim, trk_c, trk_i, vs" "CHECKPOINT" >/dev/null || bail "$dir: vacuum failed"
	[[ $dir == up ]] || old_verify_fns "$p"
	node_clone "$p" "$s" $((off + 1)) "hot_standby_feedback = on
wal_receiver_status_interval = 1s"
	use_root "$s" "$sroot"
	node_start "$s" || bail "$dir: standby start failed"
	stage "$dir: base backup" "$p" "$s"
	write_load "$p" 100000
	[[ $dir == up ]] || REFUSED_KINDS='bm25 col key-cut key-auto track-id'
	stage "$dir: writes" "$p" "$s"
	maintain "$p"
	stage "$dir: maintenance" "$p" "$s"
	if [[ $dir == up ]]; then
		node_stop "$p"
		use_root "$p" "$newroot"
		node_start "$p" || bail "$dir: the updated primary does not start"
		check "$dir: the standby follows the updated primary" catchup "$p" "$s"
		write_load "$p" 200000
		maintain "$p"
		compat "$dir: after the primary update" primary "$p"
		stage "$dir: both updated" "$p" "$s"
	fi
	node_stop "$s"
	node_stop "$p"
}

run up "$oldroot" "$newroot" 40
run down "$newroot" "$oldroot" 44
plan "$NTEST"
finish
