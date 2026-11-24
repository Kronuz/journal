#!/bin/sh
# journal_bench's append rate before and after flush modes: main's bench (one build, before
# modes) against this checkout's in full and in plain, interleaved round by round on one disk.
#
#   bench/flush_mode_ab.sh build [BASE_SOURCE]   Release builds of journal_bench under build-ab/;
#                                                BASE_SOURCE defaults to $JOB_REF_MAIN
#   bench/flush_mode_ab.sh run [ROUNDS]          ROUNDS rounds (default 7); prints every row,
#                                                then per variant and batch size the medians
#
# Run both from the repository root: each bench writes under .scratch/ there.
set -eu
root=$(pwd)
out="$root/build-ab"

case "${1:-}" in
build)
	base="${2:-${JOB_REF_MAIN:?BASE_SOURCE or JOB_REF_MAIN}}"
	for side in base head; do
		source=$root; [ "$side" = base ] && source=$base
		cmake -S "$source" -B "$out/$side" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
		cmake --build "$out/$side" --target journal_bench -j4 >/dev/null
	done
	echo "built: base $(git -C "$base" rev-parse --short HEAD), head $(git -C "$root" rev-parse --short HEAD)"
	;;
run)
	rounds="${2:-7}"
	rows="$out/rows.csv"
	: > "$rows"
	i=0
	while [ "$i" -lt "$rounds" ]; do
		# Rotate the order each round so no variant always runs first.
		case $((i % 3)) in
		0) order="main full plain" ;;
		1) order="full plain main" ;;
		*) order="plain main full" ;;
		esac
		for variant in $order; do
			case "$variant" in
			main) "$out/base/journal_bench" > "$out/last.txt" ;;
			*) "$out/head/journal_bench" "$variant" > "$out/last.txt" ;;
			esac
			grep '^[0-9]' "$out/last.txt" | sed "s/^/$variant,$i,/" >> "$rows"
		done
		i=$((i + 1))
	done
	echo "variant,round,ops/group,payload_bytes,wall_s,cpu_s,ops/s,p50_batch_ms,p99_batch_ms"
	cat "$rows"
	echo "# medians over $rounds rounds: variant, ops/group, ops/s, p50_batch_ms"
	for variant in main full plain; do
		for ops in 1 32 256; do
			rate=$(awk -F, -v v="$variant" -v o="$ops" '$1 == v && $3 == o { print $7 }' "$rows" | sort -g | awk '{ a[NR] = $1 } END { print a[int((NR + 1) / 2)] }')
			p50=$(awk -F, -v v="$variant" -v o="$ops" '$1 == v && $3 == o { print $8 }' "$rows" | sort -g | awk '{ a[NR] = $1 } END { print a[int((NR + 1) / 2)] }')
			echo "$variant,$ops,$rate,$p50"
		done
	done
	;;
*)
	echo "usage: bench/flush_mode_ab.sh build [BASE_SOURCE] | run [ROUNDS]" >&2
	exit 2
	;;
esac
