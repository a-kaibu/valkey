#!/usr/bin/env tclsh
# ZVSET ZVQUERY benchmark: selectivity, filter count/dimension, LIMIT.
#
# Loads N members (uniform dim0 over 0..100 via spread genvec scaled),
# then sweeps selectivity 100/10/1/0.1/0.01% on dim 0, filter counts
# 1/2/3/5, target dimension first/last, and LIMIT 10/100/1000 effects.
# Reports scanned N, matched M, and time.
#
# Usage:
#   tclsh utils/zv_query_benchmark.tcl [--port 6379] [--n 100000]
#       [--dims 4] [--batch 5000] [--out results.tsv] [--json results.jsonl]
#
# SPDX-License-Identifier: BSD-3-Clause

package require Tcl 8.6

set here [file dirname [info script]]
source [file join $here zv_bench_lib.tcl]

array set opt {port 6379 n 100000 dims 4 batch 5000 repeats 5 out "" json ""}
for {set i 0} {$i < $argc} {incr i} {
    set a [lindex $argv $i]
    switch -- $a {
        --port {incr i; set opt(port) [lindex $argv $i]}
        --n {incr i; set opt(n) [lindex $argv $i]}
        --dims {incr i; set opt(dims) [lindex $argv $i]}
        --batch {incr i; set opt(batch) [lindex $argv $i]}
        --repeats {incr i; set opt(repeats) [lindex $argv $i]}
        --out {incr i; set opt(out) [lindex $argv $i]}
        --json {incr i; set opt(json) [lindex $argv $i]}
        default {puts stderr "unknown arg: $a"; exit 2}
    }
}

proc time_query {fd args} {
    set t0 [clock microseconds]
    set r [zvbench::req $fd ZVQUERY {*}$args]
    set us [expr {[clock microseconds] - $t0}]
    return [list $us [llength $r]]
}

proc qload_cmd {key dims i} {
    return [list ZVADD $key [zvbench::genvec $i $dims 0 rand_uniform] [zvbench::member $i]]
}

set fd [zvbench::connect $opt(port)]
set N $opt(n)
set dims $opt(dims)
zvbench::reset_results

puts "ZV query benchmark: N=$N dims=$dims repeats=$opt(repeats)"

for {set rep 0} {$rep < $opt(repeats)} {incr rep} {
puts "--- repeat [expr {$rep + 1}]/$opt(repeats) ---"
set key "zvquery:d$dims"
zvbench::isolate_key $fd $key
# Independent dims (rand_uniform) so multi-filter conjunctions reduce
# selectivity multiplicatively. dim0 is uniform over 0..100.
zvbench::run_batches $fd $N $opt(batch) [list qload_cmd $key $dims]

# Selectivity sweep on dim 0: fraction f of the 0..100 span.
foreach f {100 10 1 0.1 0.01} {
    set span [expr {100.0 * $f / 100.0}]
    set lo [format %.2f [expr {50.0 - $span / 2}]]
    set hi [format %.2f [expr {50.0 + $span / 2}]]
    lassign [time_query $fd $key FILTER 0 $lo $hi] us m
    puts [format "sel=%-5s%% dim0 M=%-7d %8.1f ms" $f $m [expr {$us / 1000.0}]]
    zvbench::row $dims spread QUERY-sel$f [expr {$m / ($us / 1000000.0)}] "" "" \
        [dict create selectivity $f matched $m scanned $N]
}

# Filter count sweep: 10% window per dimension, independent dims, so
# each added filter multiplies selectivity by ~0.1.
for {set nf 1} {$nf <= 5} {incr nf} {
    if {$nf > $dims} continue
    set q [list $key]
    for {set d 0} {$d < $nf} {incr d} {
        lappend q FILTER $d 45 55
    }
    lassign [time_query $fd {*}$q] us m
    puts [format "filters=%d M=%-7d %8.1f ms" $nf $m [expr {$us / 1000.0}]]
    zvbench::row $dims spread QUERY-F$nf [expr {$m / ($us / 1000000.0)}] "" "" \
        [dict create filters $nf matched $m scanned $N]
}

# Target dimension: first vs last (same selectivity window).
foreach {label dim} [list first 0 last [expr {$dims - 1}]] {
    lassign [time_query $fd $key FILTER $dim 49.5 50.5] us m
    puts [format "dim-%-5s M=%-7d %8.1f ms" $label $m [expr {$us / 1000.0}]]
    zvbench::row $dims spread QUERY-dim-$label [expr {$m / ($us / 1000000.0)}] "" "" \
        [dict create matched $m scanned $N]
}

# LIMIT effect on a 10% query.
foreach lim {10 100 1000} {
    lassign [time_query $fd $key FILTER 0 45 55 LIMIT 0 $lim] us m
    puts [format "limit=%-4d M=%-7d %8.1f ms" $lim $m [expr {$us / 1000.0}]]
    zvbench::row $dims spread QUERY-limit$lim [expr {$m / ($us / 1000000.0)}] "" "" \
        [dict create limit $lim matched $m scanned $N]
}
} ;# end repeat loop

zvbench::collapse_results
zvbench::print_medians

close $fd

if {$opt(out) ne ""} {
    zvbench::write_tsv $opt(out)
}
if {$opt(json) ne ""} {
    zvbench::write_jsonl $opt(json) $N $opt(batch) 1
}
