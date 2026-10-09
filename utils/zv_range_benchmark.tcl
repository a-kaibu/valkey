#!/usr/bin/env tclsh
# ZVSET range benchmark: rank/score/lex ranges, COUNT, STORE, REMRANGE.
#
# Per (distribution, dimension): loads N members once, then sweeps
# result sizes M in --msizes for rank ranges, score ranges (narrow via
# a sampled center vector, wide via -/+), REV, LIMIT, WITHSCORES,
# ZVCOUNT/ZVLEXCOUNT consistency, ZVRANGESTORE materialization, and
# REMRANGEBYRANK/SCORE/LEX on scratch copies.
#
# Usage:
#   tclsh utils/zv_range_benchmark.tcl [--port 6379] [--n 100000]
#       [--dims "4 16"] [--dists "spread shared"] [--msizes "1 10 100 1000 10000"]
#       [--batch 5000] [--out results.tsv] [--json results.jsonl]
#
# SPDX-License-Identifier: BSD-3-Clause

package require Tcl 8.6

set here [file dirname [info script]]
source [file join $here zv_bench_lib.tcl]

array set opt {port 6379 n 100000 dims {4 16} dists {spread shared} msizes {1 10 100 1000 10000} batch 5000 out "" json ""}
for {set i 0} {$i < $argc} {incr i} {
    set a [lindex $argv $i]
    switch -- $a {
        --port {incr i; set opt(port) [lindex $argv $i]}
        --n {incr i; set opt(n) [lindex $argv $i]}
        --dims {incr i; set opt(dims) [lindex $argv $i]}
        --dists {incr i; set opt(dists) [lindex $argv $i]}
        --msizes {incr i; set opt(msizes) [lindex $argv $i]}
        --batch {incr i; set opt(batch) [lindex $argv $i]}
        --out {incr i; set opt(out) [lindex $argv $i]}
        --json {incr i; set opt(json) [lindex $argv $i]}
        default {puts stderr "unknown arg: $a"; exit 2}
    }
}

# Sample the vector at rank N/2 to center a narrow score window on real data.
proc center_vec {fd key n dims} {
    set r [zvbench::req $fd ZVRANGE $key [expr {$n / 2}] [expr {$n / 2}] WITHSCORES]
    return [lindex $r 1]
}

# Build a genuinely narrow lexicographic slice around the center
# vector: lo is the center itself, hi raises only the last dimension.
# (Any eps on leading dims, or -inf/+inf there, makes the bound useless:
# lexicographic comparison short-circuits before the constrained dim.
# The matched count M is measured and recorded per condition.)
proc narrow_bounds {center dims eps dist} {
    set cv [split $center "#"]
    set lo $center
    set hi {}
    for {set d 0} {$d < $dims} {incr d} {
        if {$d == $dims - 1} {
            lappend hi [format %.2f [expr {[lindex $cv $d] + 1.0}]]
        } else {
            lappend hi [lindex $cv $d]
        }
    }
    return [list $lo [join $hi "#"]]
}

proc bench_range {fd key M withscores} {
    set cmds {}
    for {set s 0} {$s < 100} {incr s} {
        if {$withscores} {
            lappend cmds [list ZVRANGE $key 0 [expr {$M - 1}] WITHSCORES]
        } else {
            lappend cmds [list ZVRANGE $key 0 [expr {$M - 1}]]
        }
    }
    set ms [zvbench::pipeline $fd $cmds]
    return [expr {100 / ($ms / 1000.0)}]
}

proc bench_byscore {fd key min max withscores rev {iters 100}} {
    set cmds {}
    for {set s 0} {$s < $iters} {incr s} {
        set c [list ZVRANGE $key $min $max BYSCORE]
        if {$rev} {lappend c REV}
        if {$withscores} {lappend c WITHSCORES}
        lappend cmds $c
    }
    set ms [zvbench::pipeline $fd $cmds]
    return [expr {$iters / ($ms / 1000.0)}]
}

set fd [zvbench::connect $opt(port)]
set N $opt(n)
set B $opt(batch)
zvbench::reset_results

puts "ZV range benchmark: N=$N batch=$B dims=($opt(dims)) dists=($opt(dists))"

foreach dist $opt(dists) {
    foreach dims $opt(dims) {
        set key "zvrange:$dist:d$dims"
        zvbench::isolate_key $fd $key
        zvbench::run_batches $fd $N $B [list zvbench::zvadd_cmd $key $dims 0 $dist]

        foreach M $opt(msizes) {
            if {$M > $N} continue
            foreach ws {0 1} {
                set ops [bench_range $fd $key $M $ws]
                set op "RANGE-M$M[expr {$ws ? "+scores" : ""}]"
                puts [format "%-8s dims=%-3s %-22s %10.0f ops/sec" $dist $dims $op $ops]
                zvbench::row $dims $dist $op $ops "" "" [dict create M $M]
            }
        }

        # Score ranges: narrow window around real data + wide open range.
        set center [center_vec $fd $key $N $dims]
        lassign [narrow_bounds $center $dims 1.0 $dist] nmin nmax
        set narrow_n [zvbench::req $fd ZVCOUNT $key $nmin $nmax]
        set wide_n [zvbench::req $fd ZVCOUNT $key - +]
        foreach {label min max rev actual iters} [list \
            narrow $nmin $nmax 0 $narrow_n 100 \
            narrow-rev $nmax $nmin 1 $narrow_n 100 \
            wide - + 0 $wide_n 10 \
            wide-rev + - 1 $wide_n 10] {
            foreach ws {0 1} {
                set ops [bench_byscore $fd $key $min $max $ws $rev $iters]
                set op "BYSCORE-$label[expr {$ws ? "+scores" : ""}]"
                puts [format "%-8s dims=%-3s %-22s %10.0f ops/sec" $dist $dims $op $ops]
                zvbench::row $dims $dist $op $ops "" "" [dict create M $actual]
            }
        }
        # COUNT must not grow with M: time it directly.
        foreach {label min max} [list narrow $nmin $nmax wide - +] {
            set cmds {}
            for {set s 0} {$s < 1000} {incr s} {
                lappend cmds [list ZVCOUNT $key $min $max]
            }
            set ms [zvbench::pipeline $fd $cmds]
            set ops [expr {1000 / ($ms / 1000.0)}]
            puts [format "%-8s dims=%-3s %-22s %10.0f ops/sec" $dist $dims COUNT-$label $ops]
            zvbench::row $dims $dist COUNT-$label $ops "" ""
        }
        # LIMIT with a large offset over the wide range.
        set cmds {}
        for {set s 0} {$s < 100} {incr s} {
            lappend cmds [list ZVRANGE $key - + BYSCORE LIMIT [expr {$N / 2}] 100]
        }
        set ms [zvbench::pipeline $fd $cmds]
        puts [format "%-8s dims=%-3s %-22s %10.0f ops/sec" $dist $dims LIMIT-large-off [expr {100 / ($ms / 1000.0)}]]
        zvbench::row $dims $dist LIMIT-large-off [expr {100 / ($ms / 1000.0)}] "" ""

        # RANGESTORE materialization (M=1000).
        set cmds {}
        for {set s 0} {$s < 20} {incr s} {
            lappend cmds [list ZVRANGESTORE zvrange:dst $key 0 999]
        }
        set ms [zvbench::pipeline $fd $cmds]
        puts [format "%-8s dims=%-3s %-22s %10.0f ops/sec" $dist $dims RANGESTORE-M1000 [expr {20 / ($ms / 1000.0)}]]
        zvbench::row $dims $dist RANGESTORE-M1000 [expr {20 / ($ms / 1000.0)}] "" ""
        zvbench::req $fd DEL zvrange:dst

        # Destructive deletes run on a scratch copy (COPY never
        # overwrites: DEL first. COPY cost excluded from timings).
        # Single-shot deletes of growing size (copy rebuilt each time).
        foreach M {100 1000 10000} {
            if {$M > $N} continue
            zvbench::req $fd DEL zvrange:work
            zvbench::req $fd COPY $key zvrange:work
            set t0 [clock microseconds]
            zvbench::req $fd ZVREMRANGEBYRANK zvrange:work 0 [expr {$M - 1}]
            set us [expr {[clock microseconds] - $t0}]
            puts [format "%-8s dims=%-3s %-22s %10.0f ops/sec" $dist $dims REMRANGEBYRANK-M$M [expr {$M / ($us / 1000000.0)}]]
            zvbench::row $dims $dist REMRANGEBYRANK-M$M [expr {$M / ($us / 1000000.0)}] "" ""
        }
        zvbench::req $fd DEL zvrange:work
        zvbench::req $fd COPY $key zvrange:work
        set t0 [clock microseconds]
        set deln [zvbench::req $fd ZVREMRANGEBYSCORE zvrange:work - +]
        set us [expr {[clock microseconds] - $t0}]
        puts [format "%-8s dims=%-3s %-22s %10.0f ops/sec (deleted %s)" $dist $dims REMRANGEBYSCORE-all \
            [expr {$deln / ($us / 1000000.0)}] $deln]
        zvbench::row $dims $dist REMRANGEBYSCORE-all [expr {$deln / ($us / 1000000.0)}] "" ""
        zvbench::req $fd DEL zvrange:work
    }
}

close $fd

if {$opt(out) ne ""} {
    zvbench::write_tsv $opt(out)
}
if {$opt(json) ne ""} {
    zvbench::write_jsonl $opt(json) $N $B 1
}
