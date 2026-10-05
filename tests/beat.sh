#!/bin/sh
# beat.sh DIR SECONDS [IDLE]: a log line appended every 2 s and a file
# made and removed every 10 s, no fsync, for SECONDS; then IDLE seconds of
# nothing.  Prints what vfs.gefs counted in each phase: how often the
# commit policy (vfs.gefs.interval and the syncer) woke the disk.
d=$1/beat
secs=${2:-240}
idle=${3:-0}
names="commits skipped flushes writes commits_sync commits_age commits_dirty commits_space commits_lazy"

snap() {
	for n in $names; do
		printf '%s ' "$(sysctl -n vfs.gefs.$n 2>/dev/null || echo 0)"
	done
}

report() {
	# $1: label, $2: before, $3: after
	echo "$1" "$2" "$3" | awk -v names="$names" '{
		n = split(names, nm, " ");
		printf "%s:", $1;
		for (i = 1; i <= n; i++)
			printf " %s %d", nm[i], $(i + 1 + n) - $(i + 1);
		printf "\n";
	}'
}

mkdir -p "$d" || exit 1
s0=$(snap)
i=0
end=$(($(date +%s) + secs))
while [ "$(date +%s)" -lt "$end" ]; do
	echo "line $i $(date)" >> "$d/log"
	if [ $((i % 5)) -eq 0 ]; then
		touch "$d/tmp.$i"
		rm -f "$d/tmp.$((i - 5))"
	fi
	i=$((i + 1))
	sleep 2
done
s1=$(snap)
report "active-${secs}s" "$s0" "$s1"
if [ "$idle" -gt 0 ]; then
	sleep "$idle"
	s2=$(snap)
	report "idle-${idle}s" "$s1" "$s2"
fi
