#!/usr/bin/env bash
# Minimal two-node profiler for the Nova-LSM lc/sc baseline comparison.
# It records Guest counters only and never inspects or configures the Hosts.

set -Eeuo pipefail

MEM01_HOST=${MEM01_HOST:-192.168.122.64}
MEM02_HOST=${MEM02_HOST:-192.168.122.71}
MEM01_JUMP=${MEM01_JUMP:-mem01}
MEM02_JUMP=${MEM02_JUMP:-mem02}
MEM01_RDMA_IP=${MEM01_RDMA_IP:-10.10.12.119}
MEM02_RDMA_IP=${MEM02_RDMA_IP:-10.10.12.120}
GUEST_USER=${GUEST_USER:-liumx}
GUEST_ROOT=${GUEST_ROOT:-/home/liumx/nova-lsm-baseline-20260821}
TRIALS=${PROFILE_TRIALS:-3}
TIMEOUT_S=${PROFILE_TIMEOUT_S:-180}
COOLDOWN_S=${PROFILE_COOLDOWN_S:-8}
TCP_PORT_BASE=${PROFILE_TCP_PORT_BASE:-12000}
RDMA_PORT_BASE=${PROFILE_RDMA_PORT_BASE:-21000}
OUTPUT_ROOT=${PROFILE_OUTPUT_ROOT:-/tmp/nova-compaction-profile-$(date +%Y%m%d-%H%M%S)}

if ! [[ $TRIALS =~ ^[1-9][0-9]*$ ]]; then
    echo "PROFILE_TRIALS must be a positive integer" >&2
    exit 2
fi

mkdir -p "$OUTPUT_ROOT"
# The VM overlays are intentionally recreated, so their host keys may differ
# between runs.  Keep the wrapper non-interactive and avoid touching the
# shared known_hosts file.
SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=10 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR)
CURRENT_RUN=

jump_for_host() {
    if [[ $1 == "$MEM01_HOST" ]]; then
        printf '%s' "$MEM01_JUMP"
    elif [[ $1 == "$MEM02_HOST" ]]; then
        printf '%s' "$MEM02_JUMP"
    else
        return 1
    fi
}

remote() {
    local host=$1
    shift
    local jump
    jump=$(jump_for_host "$host")
    ssh "${SSH_OPTS[@]}" -J "$jump" "$GUEST_USER@$host" "$@"
}

remote_script() {
    local host=$1
    shift
    local jump
    jump=$(jump_for_host "$host")
    ssh "${SSH_OPTS[@]}" -J "$jump" "$GUEST_USER@$host" bash -s -- "$@"
}

assert_guest_idle() {
    remote "$1" 'test -z "$(pgrep -x nova_server_main || true)"'
}

make_snapshot() {
    local host=$1 run=$2 pid_file=$3 label=$4
    remote_script "$host" "$run" "$pid_file" "$label" <<'REMOTE_SNAPSHOT'
set -Eeuo pipefail
run=$1
pid_file=$2
label=$3

echo "$(date +%s%N)" > "$run/metrics.$label.time_ns"
awk 'NR > 2 { gsub(":", "", $1); if ($1 != "lo") { rx += $2; tx += $10 } }
     END { printf "%.0f %.0f", rx + 0, tx + 0 }' \
    /proc/net/dev > "$run/metrics.$label.net"
awk '$3 ~ /^(vd[a-z]+|sd[a-z]+|xvd[a-z]+|nvme[0-9]+n[0-9]+|mmcblk[0-9]+)$/ {
         read += $6; write += $10
     }
     END { printf "%.0f %.0f", read + 0, write + 0 }' \
    /proc/diskstats > "$run/metrics.$label.disk"

pid=$(cat "$pid_file" 2>/dev/null || true)
if [[ -n "$pid" ]] && sudo -n test -r "/proc/$pid/stat"; then
    sudo -n awk '{ print $14, $15 }' "/proc/$pid/stat" > "$run/metrics.$label.cpu"
else
    echo '0 0' > "$run/metrics.$label.cpu"
fi
REMOTE_SNAPSHOT
}

start_server() {
    local host=$1 run=$2 server_id=$3 enable_load=$4 log=$5
    local pid_file=$6 db=$7 stoc=$8 mode=$9 all_servers=${10} rdma_port=${11}

    remote_script "$host" "$GUEST_ROOT" "$run" "$server_id" "$enable_load" \
        "$log" "$pid_file" "$db" "$stoc" "$mode" "$all_servers" "$rdma_port" <<'REMOTE_START'
set -Eeuo pipefail
root=$1
run=$2
server_id=$3
enable_load=$4
log=$5
pid_file=$6
db=$7
stoc=$8
mode=$9
all_servers=${10}
rdma_port=${11}

mkdir -p "$run/logs" "$db" "$stoc"
if [[ -e "$pid_file" ]]; then
    echo "refusing to reuse existing pid file: $pid_file" >&2
    exit 2
fi

args=(
  "--all_servers=$all_servers"
  "--server_id=$server_id"
  "--number_of_ltcs=1"
  "--ltc_config_path=$root/config/nova-2node-small-10000"
  "--db_path=$db"
  "--stoc_files_path=$stoc"
  "--enable_rdma=true"
  "--rdma_port=$rdma_port"
  "--rdma_max_msg_size=262144"
  "--rdma_max_num_sends=8"
  "--rdma_doorbell_batch_size=1"
  "--mem_pool_size_gb=1"
  "--enable_load_data=$enable_load"
  "--load_rounds=2"
  "--use_fixed_value_size=1024"
  "--ltc_num_client_workers=1"
  "--ltc_num_stocs_scatter_data_blocks=1"
  "--max_stoc_file_size_mb=4096"
  "--memtable_size_mb=1"
  "--sstable_size_mb=1"
  "--num_memtable_partitions=1"
  "--num_memtables=4"
  "--num_compaction_workers=1"
  "--num_storage_workers=1"
  "--num_rdma_fg_workers=1"
  "--num_rdma_bg_workers=1"
  "--num_log_replicas=0"
  "--num_manifest_replicas=1"
  "--num_sstable_metadata_replicas=1"
  "--num_sstable_replicas=1"
  "--l0_start_compaction_mb=0"
  "--l0_stop_write_mb=0"
  "--level=3"
  "--major_compaction_type=$mode"
  "--major_compaction_max_parallism=1"
  "--major_compaction_max_tables_in_a_set=15"
  "--enable_lookup_index=false"
  "--enable_range_index=false"
  "--enable_subrange=false"
  "--enable_subrange_reorg=false"
  "--enable_flush_multiple_memtables=false"
  "--use_local_disk=false"
)

cd "$root"
printf '%q ' "$root/build/nova_server_main" "${args[@]}" > "$run/command.$server_id"
echo >> "$run/command.$server_id"
sudo -n bash -c '
  log=$1
  pid_file=$2
  shift 2
  # Nova writes its console logger through std::cout; line-buffer it so the
  # profiler can collect the final Guest-side compaction records before stop.
  nohup stdbuf -oL -eL "$@" > "$log" 2>&1 < /dev/null &
  echo $! > "$pid_file"
' bash "$log" "$pid_file" "$root/build/nova_server_main" "${args[@]}"

pid=$(cat "$pid_file")
for _ in $(seq 1 20); do
    sudo -n kill -0 "$pid" 2>/dev/null && exit 0
    sleep 0.1
done
echo "Nova process did not remain alive (pid $pid)" >&2
exit 1
REMOTE_START
}

stop_server() {
    local host=$1 pid_file=$2
    remote_script "$host" "$pid_file" <<'REMOTE_STOP'
set -Eeuo pipefail
pid=$(cat "$1" 2>/dev/null || true)
if [[ -z "$pid" || ! "$pid" =~ ^[0-9]+$ ]]; then
    exit 0
fi
if sudo -n kill -0 "$pid" 2>/dev/null; then
    sudo -n kill -TERM "$pid"
    for _ in $(seq 1 50); do
        sudo -n kill -0 "$pid" 2>/dev/null || exit 0
        sleep 0.1
    done
    sudo -n kill -KILL "$pid" 2>/dev/null || true
fi
REMOTE_STOP
}

cleanup_current() {
    [[ -z $CURRENT_RUN ]] && return 0
    stop_server "$MEM01_HOST" "$CURRENT_RUN/cn.pid" >/dev/null 2>&1 || true
    stop_server "$MEM02_HOST" "$CURRENT_RUN/sn.pid" >/dev/null 2>&1 || true
}
trap cleanup_current EXIT

copy_file() {
    remote "$1" "cat '$2'" > "$3"
}

parse_stats() {
    awk '
      /Major compaction stats,/ {
        sub(/^.*Major compaction stats,/, ""); split($0, v, ","); jobs++;
        inf += v[1]; inb += v[2]; outf += v[3]; outb += v[4]; micros += v[5];
      }
      END { printf "%d %d %.0f %d %.0f %.0f\n", jobs, inf, inb, outf, outb, micros }
    ' "$1"
}

parse_lsm() {
    awk -F, '/sstables,0,/ { l0=$3 }
             /sstables,1,/ { l1=$3; l1bytes=$4 }
             END { printf "%d %d %d\n", l0 + 0, l1 + 0, l1bytes + 0 }' "$1"
}

delta_pair() {
    awk 'NR==FNR { x=$1; y=$2; next }
         { printf "%.0f %.0f\n", $1-x, $2-y }' "$1" "$2"
}

delta_cpu_seconds() {
    local ticks
    ticks=$(getconf CLK_TCK)
    awk -v hz="$ticks" 'NR==FNR { u=$1; s=$2; next }
                         { printf "%.6f %.6f\n", ($1-u)/hz, ($2-s)/hz }' "$1" "$2"
}

run_one() {
    local mode=$1 trial=$2
    local run_index
    if [[ $mode == lc ]]; then
        run_index=$((trial - 1))
    else
        run_index=$((TRIALS + trial - 1))
    fi
    # Use fresh Guest TCP/RDMA control ports per trial; this avoids TIME_WAIT
    # collisions when Nova is restarted repeatedly on the same VM pair.
    local cn_port=$((TCP_PORT_BASE + 2 * run_index))
    local sn_port=$((cn_port + 1))
    local run_all_servers="${MEM01_RDMA_IP}:${cn_port},${MEM02_RDMA_IP}:${sn_port}"
    local run_rdma_port=$((RDMA_PORT_BASE + run_index))
    local run_name="nova-profile-${mode}-${trial}-$(date +%Y%m%d-%H%M%S)-$$"
    local remote_run="/home/liumx/$run_name"
    local local_run="$OUTPUT_ROOT/$mode-$trial"
    local cn_log="$local_run/ltc.log" sn_log="$local_run/stoc.log"
    local start_ms end_ms wall_ms completed=false

    mkdir -p "$local_run"
    echo "[$mode trial $trial] remote evidence: $remote_run"
    assert_guest_idle "$MEM01_HOST"
    assert_guest_idle "$MEM02_HOST"
    CURRENT_RUN=$remote_run

    for host in "$MEM01_HOST" "$MEM02_HOST"; do
        remote_script "$host" "$remote_run" <<'REMOTE_PREP'
set -Eeuo pipefail
run=$1
if [[ -e "$run" ]]; then
    echo "refusing to reuse existing run directory: $run" >&2
    exit 2
fi
mkdir -p "$run/logs" "$run/db" "$run/stoc"
REMOTE_PREP
    done

    make_snapshot "$MEM01_HOST" "$remote_run" "$remote_run/cn.pid" start
    make_snapshot "$MEM02_HOST" "$remote_run" "$remote_run/sn.pid" start
    start_ms=$(date +%s%3N)
    start_server "$MEM02_HOST" "$remote_run" 1 false \
        "$remote_run/logs/stoc.log" "$remote_run/sn.pid" \
        "$remote_run/db" "$remote_run/stoc" "$mode" "$run_all_servers" "$run_rdma_port"
    sleep 2
    start_server "$MEM01_HOST" "$remote_run" 0 true \
        "$remote_run/logs/ltc.log" "$remote_run/cn.pid" \
        "$remote_run/db" "$remote_run/stoc" "$mode" "$run_all_servers" "$run_rdma_port"

    local deadline=$((SECONDS + TIMEOUT_S))
    while (( SECONDS < deadline )); do
        if ! remote "$MEM01_HOST" "pid=\$(cat '$remote_run/cn.pid' 2>/dev/null || true); [[ -n \"\$pid\" ]] && sudo -n kill -0 \"\$pid\" 2>/dev/null"; then
            echo "[$mode trial $trial] CN Guest process exited before completion" >&2
            return 1
        fi
        if ! remote "$MEM02_HOST" "pid=\$(cat '$remote_run/sn.pid' 2>/dev/null || true); [[ -n \"\$pid\" ]] && sudo -n kill -0 \"\$pid\" 2>/dev/null"; then
            echo "[$mode trial $trial] SN Guest process exited before completion" >&2
            return 1
        fi
        if remote "$MEM01_HOST" "grep -aq 'Complete Load took' '$remote_run/logs/ltc.log'"; then
            completed=true
            break
        fi
        sleep 1
    done
    [[ $completed == true ]] || { echo "[$mode trial $trial] timed out" >&2; return 1; }

    end_ms=$(date +%s%3N)
    wall_ms=$((end_ms - start_ms))
    sleep 1
    make_snapshot "$MEM01_HOST" "$remote_run" "$remote_run/cn.pid" end
    make_snapshot "$MEM02_HOST" "$remote_run" "$remote_run/sn.pid" end

    copy_file "$MEM01_HOST" "$remote_run/logs/ltc.log" "$cn_log"
    copy_file "$MEM02_HOST" "$remote_run/logs/stoc.log" "$sn_log"
    for spec in "${MEM01_HOST}:cn" "${MEM02_HOST}:sn"; do
        local host=${spec%%:*} role=${spec##*:}
        for metric in time_ns net disk cpu; do
            copy_file "$host" "$remote_run/metrics.start.$metric" "$local_run/$role.start.$metric"
            copy_file "$host" "$remote_run/metrics.end.$metric" "$local_run/$role.end.$metric"
        done
    done

    stop_server "$MEM01_HOST" "$remote_run/cn.pid"
    stop_server "$MEM02_HOST" "$remote_run/sn.pid"
    CURRENT_RUN=
    # Allow Guest RDMA/QP teardown to settle before the next two-node start.
    sleep "$COOLDOWN_S"

    local source_log=$cn_log
    [[ $mode == sc ]] && source_log=$sn_log
    local jobs input_files input_bytes output_files output_bytes compaction_us
    local l0_files l1_files l1_bytes cn_user cn_sys sn_user sn_sys
    local cn_rx cn_tx sn_rx sn_tx cn_read cn_write sn_read sn_write rounds
    read -r jobs input_files input_bytes output_files output_bytes compaction_us < <(parse_stats "$source_log")
    read -r l0_files l1_files l1_bytes < <(parse_lsm "$cn_log")
    read -r cn_user cn_sys < <(delta_cpu_seconds "$local_run/cn.start.cpu" "$local_run/cn.end.cpu")
    read -r sn_user sn_sys < <(delta_cpu_seconds "$local_run/sn.start.cpu" "$local_run/sn.end.cpu")
    read -r cn_rx cn_tx < <(delta_pair "$local_run/cn.start.net" "$local_run/cn.end.net")
    read -r sn_rx sn_tx < <(delta_pair "$local_run/sn.start.net" "$local_run/sn.end.net")
    read -r cn_read cn_write < <(delta_pair "$local_run/cn.start.disk" "$local_run/cn.end.disk")
    read -r sn_read sn_write < <(delta_pair "$local_run/sn.start.disk" "$local_run/sn.end.disk")
    rounds=$(grep -a -c 'Completed loading data 10000' "$cn_log" || true)

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$mode" "$trial" "$wall_ms" "$rounds" "$jobs" "$input_files" \
        "$input_bytes" "$output_files" "$output_bytes" "$compaction_us" \
        "$cn_user" "$cn_sys" "$sn_user" "$sn_sys" "$cn_rx" "$cn_tx" \
        "$sn_rx" "$sn_tx" "$((cn_read * 512))" "$((cn_write * 512))" \
        "$((sn_read * 512))" "$((sn_write * 512))" "$l0_files" "$l1_files" "$l1_bytes" \
        >> "$OUTPUT_ROOT/results.csv"
    echo "[$mode trial $trial] wall=${wall_ms}ms jobs=$jobs input=${input_bytes}B output=${output_bytes}B"
}

echo 'mode,trial,wall_ms,load_rounds,major_jobs,input_files,input_bytes,output_files,output_bytes,compaction_us,cn_user_s,cn_sys_s,sn_user_s,sn_sys_s,cn_rx_bytes,cn_tx_bytes,sn_rx_bytes,sn_tx_bytes,cn_disk_read_bytes,cn_disk_write_bytes,sn_disk_read_bytes,sn_disk_write_bytes,l0_files,l1_files,l1_bytes' > "$OUTPUT_ROOT/results.csv"

for mode in lc sc; do
    for trial in $(seq 1 "$TRIALS"); do
        run_one "$mode" "$trial"
    done
done

echo "results: $OUTPUT_ROOT/results.csv"
