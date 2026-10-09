#!/usr/bin/env tclsh
# ZVSET PoC benchmark: basic commands.
#
# Measures, per (distribution, dimension):
#   ZVADD insert/update/no-op-update/hot-update/NX/XX/GT/LT, ZVINCRBY,
#   ZVSCORE, ZVMSCORE, ZVRANK, ZVREVRANK, ZVREM, ZVCARD,
#   ZVRANGE 0 99 [+WITHSCORES], plus a ZSET baseline.
#
# See zv_bench_lib.tcl for distributions and methodology notes.
#
# Usage:
#   tclsh utils/zv_benchmark.tcl [--port 6379] [--n 1000000] \
#       [--dims "1 2 4 8 16"] [--dists "spread shared"] [--batch 5000] \
#       [--samples 5000] [--rtts 1000] [--repeats 5]
#       [--out results.tsv] [--json results.jsonl]
#
# SPDX-License-Identifier: BSD-3-Clause

package require Tcl 8.6

set here [file dirname [info script]]
source [file join $here zv_bench_lib.tcl]

array set opt {port 6379 n 1000000 dims {1 2 4 8 16} dists {spread shared} batch 5000 samples 5000 rtts 1000 repeats 5 out "" json ""}
for {set i 0} {$i < $argc} {incr i} {
    set a [lindex $argv $i]
    switch -- $a {
        --port {incr i; set opt(port) [lindex $argv $i]}
        --n {incr i; set opt(n) [lindex $argv $i]}
        --dims {incr i; set opt(dims) [lindex $argv $i]}
        --dists {incr i; set opt(dists) [lindex $argv $i]}
        --batch {incr i; set opt(batch) [lindex $argv $i]}
        --samples {incr i; set opt(samples) [lindex $argv $i]}
        --rtts {incr i; set opt(rtts) [lindex $argv $i]}
        --repeats {incr i; set opt(repeats) [lindex $argv $i]}
        --out {incr i; set opt(out) [lindex $argv $i]}
        --json {incr i; set opt(json) [lindex $argv $i]}
        default {puts stderr "unknown arg: $a"; exit 2}
    }
}

proc zadd_cmd {key g i} {
    if {$g == 0} {
        set v [expr {(($i * 31) % 10000) / 100.0}]
    } else {
        set v [expr {(($i * 31 + 17) % 10000) / 100.0}]
    }
    return [list ZADD $key [format %.2f $v] [zvbench::member $i]]
}

set fd [zvbench::connect $opt(port)]
set N $opt(n)
set B $opt(batch)
zvbench::reset_results

puts "ZV basic benchmark: N=$N batch=$B dims=($opt(dims)) dists=($opt(dists)) repeats=$opt(repeats) port=$opt(port)"

# Repeat the whole condition matrix with rotated order; results are
# collapsed to medians (min/max kept) at the end.
set conds {}
foreach dist $opt(dists) {
    foreach dims $opt(dims) {
        lappend conds [list $dist $dims]
    }
}
for {set rep 0} {$rep < $opt(repeats)} {incr rep} {
puts "--- repeat [expr {$rep + 1}]/$opt(repeats) ---"
foreach cond [zvbench::rotated $conds $rep] {
lassign $cond dist dims
        set key "zvbench:$dist:d$dims"
        zvbench::isolate_key $fd $key
        set mk [list zvbench::zvadd_cmd $key $dims 0 $dist]

        lassign [zvbench::run_batches $fd $N $B [list zvbench::zvadd_cmd $key $dims 0 $dist]] med tot nb
        puts [format "%-8s dims=%-3s ZVADD-insert   median %10.0f ops/sec" $dist $dims $med]
        zvbench::row $dims $dist ZVADD-insert $med "" ""
        lassign [zvbench::run_batches $fd $N $B [list zvbench::zvadd_cmd $key $dims 1 $dist]] med tot nb
        puts [format "%-8s dims=%-3s ZVADD-update   median %10.0f ops/sec" $dist $dims $med]
        zvbench::row $dims $dist ZVADD-update $med "" ""

        # Re-applying the current vectors: score-equal fast path, no tree write.
        lassign [zvbench::run_batches $fd $N $B [list zvbench::zvadd_cmd $key $dims 1 $dist]] med tot nb
        puts [format "%-8s dims=%-3s ZVADD-noop     median %10.0f ops/sec" $dist $dims $med]
        zvbench::row $dims $dist ZVADD-noop $med "" ""

        # Hot members: 100 members updated round-robin (cache-resident).
        set hot {}
        for {set i 0} {$i < $N} {incr i} {
            lappend hot [list ZVADD $key [zvbench::genvec [expr {$i % 100}] $dims 2 $dist] \
                [zvbench::member [expr {$i % 100}]]]
        }
        set ms [zvbench::pipeline $fd $hot]
        puts [format "%-8s dims=%-3s ZVADD-hot      median %10.0f ops/sec" $dist $dims [expr {$N / ($ms / 1000.0)}]]
        zvbench::row $dims $dist ZVADD-hot [expr {$N / ($ms / 1000.0)}] "" ""

        # Flag / INCR variants over all members.
        set nxcmds {}
        set xxcmds {}
        set gtcmds {}
        set incrcmds {}
        for {set i 0} {$i < $N} {incr i} {
            set m [zvbench::member $i]
            set v [zvbench::genvec $i $dims 3 $dist]
            lappend nxcmds [list ZVADD $key NX $v $m]
            lappend xxcmds [list ZVADD $key XX $v $m]
            lappend gtcmds [list ZVADD $key GT $v $m]
            lappend incrcmds [list ZVINCRBY $key $v $m]
        }
        foreach {name cmds} [list ZVADD-NX $nxcmds ZVADD-XX $xxcmds ZVADD-GT $gtcmds ZVINCRBY $incrcmds] {
            set ms [zvbench::pipeline $fd $cmds]
            set ops [expr {$N / ($ms / 1000.0)}]
            puts [format "%-8s dims=%-3s %-12s %10.0f ops/sec" $dist $dims $name $ops]
            zvbench::row $dims $dist $name $ops "" ""
        }

        # Point lookups.
        set S $opt(samples)
        set rankcmds {}
        set scorecmds {}
        set mscorecmds {}
        for {set s 0} {$s < $S} {incr s} {
            set m [zvbench::member [expr {($s * 7919) % $N}]]
            lappend rankcmds [list ZVRANK $key $m]
            lappend scorecmds [list ZVSCORE $key $m]
        }
        for {set s 0} {$s < $S} {incr s 10} {
            set batch [list ZVMSCORE $key]
            for {set k 0} {$k < 10} {incr k} {
                lappend batch [zvbench::member [expr {(($s + $k) * 7919) % $N}]]
            }
            lappend mscorecmds $batch
        }
        foreach {name cmds} [list ZVRANK $rankcmds ZVSCORE $scorecmds] {
            set ms [zvbench::pipeline $fd $cmds]
            set ops [expr {$S / ($ms / 1000.0)}]
            lassign [zvbench::sample_rtt $fd [lrange $cmds 0 [expr {$opt(rtts) - 1}]]] avg p50 p99
            puts [format "%-8s dims=%-3s %-12s %10.0f ops/sec  rtt avg=%.1fus p50=%.1fus p99=%.1fus" \
                $dist $dims $name $ops $avg $p50 $p99]
            zvbench::row $dims $dist $name $ops $p50 $p99
        }
        set ms [zvbench::pipeline $fd $mscorecmds]
        set ops [expr {[llength $mscorecmds] / ($ms / 1000.0)}]
        puts [format "%-8s dims=%-3s %-12s %10.0f ops/sec" $dist $dims ZVMSCORE $ops]
        zvbench::row $dims $dist ZVMSCORE $ops "" ""
        # ZVREVRANK reuses the rank path; sample a slice for the delta.
        set revcmds {}
        foreach c [lrange $rankcmds 0 999] {
            lappend revcmds [list ZVREVRANK [lindex $c 1] [lindex $c 2]]
        }
        set ms [zvbench::pipeline $fd $revcmds]
        puts [format "%-8s dims=%-3s %-12s %10.0f ops/sec" $dist $dims ZVREVRANK [expr {1000 / ($ms / 1000.0)}]]
        zvbench::row $dims $dist ZVREVRANK [expr {1000 / ($ms / 1000.0)}] "" ""

        # Small ranges.
        foreach {name extra} {ZVRANGE {} ZVRANGE-WITHSCORES {WITHSCORES}} {
            set cmds {}
            for {set s 0} {$s < 100} {incr s} {
                if {$extra eq ""} {
                    lappend cmds [list ZVRANGE $key 0 99]
                } else {
                    lappend cmds [list ZVRANGE $key 0 99 WITHSCORES]
                }
            }
            set ms [zvbench::pipeline $fd $cmds]
            puts [format "%-8s dims=%-3s %-18s %10.0f ops/sec" $dist $dims $name [expr {100 / ($ms / 1000.0)}]]
            zvbench::row $dims $dist $name [expr {100 / ($ms / 1000.0)}] "" ""
        }

        # ZVREM: delete half, then the rest (fresh reload per half avoided;
        # reload once at the end for memory accounting only if needed).
        set remcmds {}
        for {set i 0} {$i < $N} {incr i 2} {
            lappend remcmds [list ZVREM $key [zvbench::member $i]]
        }
        set ms [zvbench::pipeline $fd $remcmds]
        puts [format "%-8s dims=%-3s %-12s %10.0f ops/sec" $dist $dims ZVREM [expr {[llength $remcmds] / ($ms / 1000.0)}]]
        zvbench::row $dims $dist ZVREM [expr {[llength $remcmds] / ($ms / 1000.0)}] "" ""

        # Memory accounting on a full key: reload, then sample.
        zvbench::isolate_key $fd $key
        zvbench::run_batches $fd $N $B [list zvbench::zvadd_cmd $key $dims 0 $dist]
        set mem [zvbench::mem_usage $fd $key]
        puts "$dist dims=$dims MEMORY USAGE $key = $mem bytes"
        zvbench::row $dims $dist MEMORY-USAGE $mem "" "" [dict create memory_bytes $mem]
    }

# --- ZSET baseline (dimension 1 equivalent, every repeat) ---
zvbench::isolate_key $fd zbench
lassign [zvbench::run_batches $fd $N $B [list zadd_cmd zbench 0]] med tot nb
puts [format "ZSET      ZADD-insert    median %10.0f ops/sec" $med]
zvbench::row 1 spread ZSET-ZADD-insert $med "" ""
lassign [zvbench::run_batches $fd $N $B [list zadd_cmd zbench 1]] med tot nb
puts [format "ZSET      ZADD-update    median %10.0f ops/sec" $med]
zvbench::row 1 spread ZSET-ZADD-update $med "" ""
set S $opt(samples)
set rankcmds {}
set scorecmds {}
for {set s 0} {$s < $S} {incr s} {
    set m [zvbench::member [expr {($s * 7919) % $N}]]
    lappend rankcmds [list ZRANK zbench $m]
    lappend scorecmds [list ZSCORE zbench $m]
}
foreach {name cmds} [list ZRANK $rankcmds ZSCORE $scorecmds] {
    set ms [zvbench::pipeline $fd $cmds]
    lassign [zvbench::sample_rtt $fd [lrange $cmds 0 [expr {$opt(rtts) - 1}]]] avg p50 p99
    puts [format "ZSET      %-12s %10.0f ops/sec  rtt avg=%.1fus p50=%.1fus p99=%.1fus" \
        $name [expr {$S / ($ms / 1000.0)}] $avg $p50 $p99]
    zvbench::row 1 spread ZSET-$name [expr {$S / ($ms / 1000.0)}] $p50 $p99
}
foreach {name extra} {ZRANGE {} ZRANGE-WITHSCORES {WITHSCORES}} {
    set cmds {}
    for {set s 0} {$s < 100} {incr s} {
        if {$extra eq ""} {
            lappend cmds [list ZRANGE zbench 0 99]
        } else {
            lappend cmds [list ZRANGE zbench 0 99 WITHSCORES]
        }
    }
    set ms [zvbench::pipeline $fd $cmds]
    puts [format "ZSET      %-18s %10.0f ops/sec" $name [expr {100 / ($ms / 1000.0)}]]
    zvbench::row 1 spread ZSET-$name [expr {100 / ($ms / 1000.0)}] "" ""
}
puts "ZSET      MEMORY USAGE zbench = [zvbench::req $fd MEMORY USAGE zbench] bytes"
} ;# end repeat loop

zvbench::collapse_results
zvbench::print_medians

close $fd

if {$opt(out) ne ""} {
    zvbench::write_tsv $opt(out)
}
if {$opt(json) ne ""} {
    zvbench::write_jsonl $opt(json) $N $B 1
}
