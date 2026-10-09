#!/usr/bin/env tclsh
# ZVSET setops benchmark: UNION/INTER/DIFF/INTERCARD (+STORE).
#
# Matrix: input keys K in --keys, overlap ratios in --overlaps, N members
# per key. Overlap construction: P% of members shared across all keys
# (same names, per-key vectors), the rest unique per key.
# Aggregates SUM/MIN/MAX, INTERCARD with LIMIT 10/100, STORE
# materialization cost. Reports input total, result count, time, and
# used_memory delta.
#
# Usage:
#   tclsh utils/zv_setops_benchmark.tcl [--port 6379] [--n 100000]
#       [--dims 4] [--keys "2 4 8"] [--overlaps "0 10 50 90 100"]
#       [--batch 5000] [--out results.tsv] [--json results.jsonl]
#
# SPDX-License-Identifier: BSD-3-Clause

package require Tcl 8.6

set here [file dirname [info script]]
source [file join $here zv_bench_lib.tcl]

array set opt {port 6379 n 100000 dims 4 keys {2 4} overlaps {0 10 50 90 100} batch 5000 repeats 5 out "" json ""}
for {set i 0} {$i < $argc} {incr i} {
    set a [lindex $argv $i]
    switch -- $a {
        --port {incr i; set opt(port) [lindex $argv $i]}
        --n {incr i; set opt(n) [lindex $argv $i]}
        --dims {incr i; set opt(dims) [lindex $argv $i]}
        --keys {incr i; set opt(keys) [lindex $argv $i]}
        --overlaps {incr i; set opt(overlaps) [lindex $argv $i]}
        --batch {incr i; set opt(batch) [lindex $argv $i]}
        --repeats {incr i; set opt(repeats) [lindex $argv $i]}
        --out {incr i; set opt(out) [lindex $argv $i]}
        --json {incr i; set opt(json) [lindex $argv $i]}
        default {puts stderr "unknown arg: $a"; exit 2}
    }
}

# Load K keys with P% shared members. Shared member j of key k gets
# vector genvec(j, g=k) so aggregation is non-trivial.
proc load_overlap {fd prefix k n dims overlap} {
    set shared [expr {($n * $overlap) / 100}]
    for {set ki 0} {$ki < $k} {incr ki} {
        set key "$prefix:k$ki"
        zvbench::req $fd DEL $key
        set cmds {}
        for {set j 0} {$j < $shared} {incr j} {
            lappend cmds [list ZVADD $key [zvbench::genvec $j $dims $ki spread] [zvbench::member $j]]
            if {[llength $cmds] >= 5000} {
                zvbench::pipeline $fd $cmds
                set cmds {}
            }
        }
        for {set j $shared} {$j < $n} {incr j} {
            set m "u${ki}_[zvbench::member $j]"
            lappend cmds [list ZVADD $key [zvbench::genvec $j $dims $ki spread] $m]
            if {[llength $cmds] >= 5000} {
                zvbench::pipeline $fd $cmds
                set cmds {}
            }
        }
        if {[llength $cmds] > 0} {
            zvbench::pipeline $fd $cmds
        }
    }
}

proc time_cmd {fd args} {
    set t0 [clock microseconds]
    set r [zvbench::req $fd {*}$args]
    set us [expr {[clock microseconds] - $t0}]
    return [list $us $r]
}

set fd [zvbench::connect $opt(port)]
set N $opt(n)
set dims $opt(dims)
zvbench::reset_results

puts "ZV setops benchmark: N=$N dims=$dims keys=($opt(keys)) overlaps=($opt(overlaps)) repeats=$opt(repeats)"

set conds {}
foreach k $opt(keys) {
    foreach overlap $opt(overlaps) {
        lappend conds [list $k $overlap]
    }
}
for {set rep 0} {$rep < $opt(repeats)} {incr rep} {
puts "--- repeat [expr {$rep + 1}]/$opt(repeats) ---"
foreach cond [zvbench::rotated $conds $rep] {
lassign $cond k overlap
        set prefix "zvsetops:k$k:o$overlap"
        foreach pat [list "$prefix:*" zvsetops:dst] {
            foreach old [zvbench::req $fd KEYS $pat] {
                zvbench::req $fd DEL $old
            }
        }
        catch {zvbench::req $fd MEMORY PURGE}
        set mem0 [zvbench::used_memory $fd]
        load_overlap $fd $prefix $k $N $dims $overlap
        set keys {}
        for {set ki 0} {$ki < $k} {incr ki} {
            lappend keys "$prefix:k$ki"
        }

        # Rate denominator: total input elements scanned (K*N).
        set input_total [expr {$k * $N}]
        foreach aggr {SUM MIN MAX} {
            lassign [time_cmd $fd ZVUNION $k {*}$keys AGGREGATE $aggr] us r
            set nres [llength $r]
            puts [format "K=%d ov=%-3d UNION-%-3s %8.1f ms res=%d" $k $overlap $aggr [expr {$us / 1000.0}] $nres]
            zvbench::row $dims spread UNION-$aggr [expr {$input_total / ($us / 1000000.0)}] "" "" \
                [dict create K $k overlap $overlap result $nres]
            lassign [time_cmd $fd ZVINTER $k {*}$keys AGGREGATE $aggr] us r
            set nres [llength $r]
            puts [format "K=%d ov=%-3d INTER-%-3s %8.1f ms res=%d" $k $overlap $aggr [expr {$us / 1000.0}] $nres]
            zvbench::row $dims spread INTER-$aggr [expr {$input_total / ($us / 1000000.0)}] "" "" \
                [dict create K $k overlap $overlap result $nres]
        }
        lassign [time_cmd $fd ZVDIFF $k {*}$keys] us r
        set nres [llength $r]
        puts [format "K=%d ov=%-3d DIFF      %8.1f ms res=%d" $k $overlap [expr {$us / 1000.0}] $nres]
        zvbench::row $dims spread DIFF [expr {$input_total / ($us / 1000000.0)}] "" "" \
            [dict create K $k overlap $overlap result $nres]

        lassign [time_cmd $fd ZVINTERCARD $k {*}$keys] us r
        puts [format "K=%d ov=%-3d INTERCARD %8.1f ms res=%s (%.0f queries/sec)" \
            $k $overlap [expr {$us / 1000.0}] $r [expr {1000000.0 / $us}]]
        zvbench::row $dims spread INTERCARD [expr {1000000.0 / $us}] "" "" \
            [dict create K $k overlap $overlap result $r elapsed_ms [expr {$us / 1000.0}]]
        foreach lim {10 100} {
            lassign [time_cmd $fd ZVINTERCARD $k {*}$keys LIMIT $lim] us r
            puts [format "K=%d ov=%-3d INTERCARD-L$lim %8.1f ms res=%s (%.0f queries/sec)" \
                $k $overlap $lim [expr {$us / 1000.0}] $r [expr {1000000.0 / $us}]]
            zvbench::row $dims spread INTERCARD-L$lim [expr {1000000.0 / $us}] "" "" \
                [dict create K $k overlap $overlap result $r elapsed_ms [expr {$us / 1000.0}]]
        }

        # STORE materialization.
        set mem1 [zvbench::used_memory $fd]
        lassign [time_cmd $fd ZVUNIONSTORE zvsetops:dst $k {*}$keys] us r
        set mem2 [zvbench::used_memory $fd]
        puts [format "K=%d ov=%-3d UNIONSTORE %8.1f ms res=%s mem-delta=%dKB" \
            $k $overlap [expr {$us / 1000.0}] $r [expr {($mem2 - $mem1) / 1024}]]
        zvbench::row $dims spread UNIONSTORE [expr {$input_total / ($us / 1000000.0)}] "" "" \
            [dict create K $k overlap $overlap result $r memory_bytes [expr {$mem2 - $mem1}]]
        zvbench::req $fd DEL zvsetops:dst

        foreach old [zvbench::req $fd KEYS "$prefix:*"] {
            zvbench::req $fd DEL $old
        }
    }
}

zvbench::collapse_results
zvbench::print_medians

close $fd

if {$opt(out) ne ""} {
    zvbench::write_tsv $opt(out)
}
if {$opt(json) ne ""} {
    zvbench::write_jsonl $opt(json) $N $opt(batch) 1
}
