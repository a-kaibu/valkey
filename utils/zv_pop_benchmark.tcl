#!/usr/bin/env tclsh
# ZVSET POP benchmark: ZVPOPMIN/MAX, ZVMPOP, wake-up latency.
#
# Counts 1/10/100/1000 are measured with a fresh reload per count
# (POP drains the key). Also measures BZVPOPMIN wake-up latency:
# consumer blocks, producer ZVADDs, time from producer-flush to
# pop reply (server wake + pop path).
#
# Usage:
#   tclsh utils/zv_pop_benchmark.tcl [--port 6379] [--n 100000]
#       [--dims "4 16"] [--dists "spread shared"] [--counts "1 10 100 1000"]
#       [--batch 5000] [--out results.tsv] [--json results.jsonl]
#
# SPDX-License-Identifier: BSD-3-Clause

package require Tcl 8.6

set here [file dirname [info script]]
source [file join $here zv_bench_lib.tcl]

array set opt {port 6379 n 100000 dims {4 16} dists {spread shared} counts {1 10 100 1000} batch 5000 out "" json ""}
for {set i 0} {$i < $argc} {incr i} {
    set a [lindex $argv $i]
    switch -- $a {
        --port {incr i; set opt(port) [lindex $argv $i]}
        --n {incr i; set opt(n) [lindex $argv $i]}
        --dims {incr i; set opt(dims) [lindex $argv $i]}
        --dists {incr i; set opt(dists) [lindex $argv $i]}
        --counts {incr i; set opt(counts) [lindex $argv $i]}
        --batch {incr i; set opt(batch) [lindex $argv $i]}
        --out {incr i; set opt(out) [lindex $argv $i]}
        --json {incr i; set opt(json) [lindex $argv $i]}
        default {puts stderr "unknown arg: $a"; exit 2}
    }
}

proc reload_key {fd key n dims dist} {
    zvbench::req $fd DEL $key
    zvbench::run_batches $fd $n 5000 [list zvbench::zvadd_cmd $key $dims 0 $dist]
}

# Time a single blocking pop round-trip when data is already present
# (no actual block): measures command latency, not wake-up.
proc pop_one {fd key where} {
    set cmd [expr {$where eq "MIN" ? "ZVPOPMIN" : "ZVPOPMAX"}]
    set t0 [clock microseconds]
    set r [zvbench::req $fd $cmd $key]
    return [list [expr {[clock microseconds] - $t0}] $r]
}

set fd [zvbench::connect $opt(port)]
set N $opt(n)
set B $opt(batch)
zvbench::reset_results

puts "ZV pop benchmark: N=$N dims=($opt(dims)) dists=($opt(dists))"

foreach dist $opt(dists) {
    foreach dims $opt(dims) {
        set key "zvpop:$dist:d$dims"
        zvbench::isolate_key $fd $key

        foreach cnt $opt(counts) {
            if {$cnt > $N} continue
            foreach where {MIN MAX} {
                reload_key $fd $key $N $dims $dist
                set cmd [expr {$where eq "MIN" ? "ZVPOPMIN" : "ZVPOPMAX"}]
                set t0 [clock microseconds]
                # Single command (server does the COUNT loop internally).
                set got [zvbench::req $fd $cmd $key $cnt]
                set us [expr {[clock microseconds] - $t0}]
                set npop [expr {[llength $got] / 2}]
                puts [format "%-8s dims=%-3s ZVPOP%s-C%s %10.0f pops/sec (%d pops)" \
                    $dist $dims $where $cnt [expr {$npop / ($us / 1000000.0)}] $npop]
                zvbench::row $dims $dist ZVPOP$where-C$cnt [expr {$npop / ($us / 1000000.0)}] "" "" \
                    [dict create M $npop]
            }
        }

        # ZVMPOP across two keys (drains the first).
        reload_key $fd $key $N $dims $dist
        zvbench::req $fd COPY $key ${key}:2
        set t0 [clock microseconds]
        set got [zvbench::req $fd ZVMPOP 2 $key ${key}:2 MIN COUNT 100]
        set us [expr {[clock microseconds] - $t0}]
        puts [format "%-8s dims=%-3s %-22s %10.0f ops/sec" $dist $dims ZVMPOP-C100 [expr {100 / ($us / 1000000.0)}]]
        zvbench::row $dims $dist ZVMPOP-C100 [expr {100 / ($us / 1000000.0)}] "" ""
        zvbench::req $fd DEL ${key}:2

        # Wake-up latency: blocked consumer + producer add.
        # Single-threaded measurement: send BZVPOPMIN (nonblocking fd),
        # flush producer ZVADD, then time the blocking read.
        set cfd [socket 127.0.0.1 $opt(port)]
        fconfigure $cfd -translation binary -buffering full -blocking 0
        zvbench::req $fd DEL ${key}:w
        puts -nonewline $cfd [zvbench::pack BZVPOPMIN ${key}:w 5]
        flush $cfd
        after 200
        set t0 [clock microseconds]
        puts -nonewline $fd [zvbench::pack ZVADD ${key}:w [zvbench::genvec 0 $dims 0 $dist] wake-member]
        flush $fd
        zvbench::readReply $fd
        fconfigure $cfd -blocking 1
        set r [zvbench::readReply $cfd]
        set us [expr {[clock microseconds] - $t0}]
        close $cfd
        if {[string index $r 0] eq "-"} {error "wake-up failed: $r"}
        puts [format "%-8s dims=%-3s %-22s %.1f us wake-to-pop" $dist $dims BZVPOPMIN-wake $us]
        zvbench::row $dims $dist BZVPOPMIN-wake-us $us "" ""
        zvbench::req $fd DEL ${key}:w
    }
}

close $fd

if {$opt(out) ne ""} {
    zvbench::write_tsv $opt(out)
}
if {$opt(json) ne ""} {
    zvbench::write_jsonl $opt(json) $N $B 1
}
