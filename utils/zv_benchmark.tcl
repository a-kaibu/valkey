#!/usr/bin/env tclsh
# ZVSET PoC benchmark.
#
# Measures, for dimensions 1/2/4/8/16 (configurable):
#   ZVADD new insert, ZVADD update, ZVRANK, ZVSCORE,
#   ZVRANGE 0 99, ZVRANGE 0 99 WITHSCORES
# plus a ZSET baseline at dimension 1.
#
# Score distributions (workloads):
#   spread: every dimension varies, so tree comparisons usually settle
#           on the first dimension.
#   shared: all dimensions but the last are fixed, so comparisons walk
#           the full vector (worst-case-ish for multi-score compare and
#           long common prefixes).
#
# Bulk phases are sent in --batch sized pipelines and the median batch
# throughput is reported (more robust than one giant pipeline, and the
# client no longer buffers all N commands at once). RANK/SCORE
# throughput uses --samples pipelined lookups (default 5000); RTT
# p50/p99 uses --rtts sequential samples (default 1000).
#
# Each (dist, dims) condition is measured in isolation: all other
# benchmark keys are deleted and freed pages purged first, so memory
# pressure and cache state stay comparable across conditions.
#
# Latency p50/p99 are client-observed loopback RTT samples (server +
# stack), useful for relative comparison, not absolute server latency.
# Stays on RESP2 (no HELLO upgrade).
#
# Usage:
#   tclsh utils/zv_benchmark.tcl [--port 6379] [--n 1000000] \
#       [--dims "1 2 4 8 16"] [--dists "spread shared"] [--batch 5000] \
#       [--samples 5000] [--rtts 1000] [--out results.tsv]
#
# SPDX-License-Identifier: BSD-3-Clause

package require Tcl 8.6

array set opt {port 6379 n 1000000 dims {1 2 4 8 16} dists {spread shared} batch 5000 samples 5000 rtts 1000 out ""}
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
        --out {incr i; set opt(out) [lindex $argv $i]}
        default {puts stderr "unknown arg: $a"; exit 2}
    }
}

proc pack {args} {
    set out "*[llength $args]\r\n"
    foreach a $args {
        append out "\$[string length $a]\r\n$a\r\n"
    }
    return $out
}

# Minimal RESP2 client parser (no RESP3 maps needed).
proc readReply {fd} {
    set line [string trimright [gets $fd] \r]
    set t [string index $line 0]
    set rest [string range $line 1 end]
    switch -- $t {
        "+" - "-" {return $line}
        ":" {return $rest}
        "$" {
            if {$rest == -1} {return {}}
            set data [read $fd [expr {$rest + 2}]]
            return [string range $data 0 end-2]
        }
        "*" {
            if {$rest == -1} {return {}}
            set res {}
            for {set i 0} {$i < $rest} {incr i} {
                lappend res [readReply $fd]
            }
            return $res
        }
        default {error "bad reply: $line"}
    }
}

proc req {fd args} {
    puts -nonewline $fd [pack {*}$args]
    flush $fd
    return [readReply $fd]
}

# Pipeline a fixed list of argv-lists; returns elapsed ms.
proc pipeline {fd cmds} {
    set buf ""
    foreach c $cmds {
        append buf [pack {*}$c]
    }
    set t0 [clock microseconds]
    puts -nonewline $fd $buf
    flush $fd
    foreach c $cmds {
        set r [readReply $fd]
        if {[string index $r 0] eq "-"} {
            error "server error: $r"
        }
    }
    return [expr {([clock microseconds] - $t0) / 1000.0}]
}

# Run $total commands built by "$buildCmd $index" in $batch sized
# pipelines. Returns {median_batch_ops_per_sec total_ops_per_sec nbatches}.
proc run_batches {fd total batch buildCmd} {
    set summs 0.0
    set opslist {}
    set done 0
    while {$done < $total} {
        set n [expr {$batch < $total - $done ? $batch : $total - $done}]
        set buf ""
        for {set k 0} {$k < $n} {incr k} {
            append buf [pack {*}[{*}$buildCmd [expr {$done + $k}]]]
        }
        set t0 [clock microseconds]
        puts -nonewline $fd $buf
        flush $fd
        for {set k 0} {$k < $n} {incr k} {
            set r [readReply $fd]
            if {[string index $r 0] eq "-"} {error "server error: $r"}
        }
        set ms [expr {([clock microseconds] - $t0) / 1000.0}]
        set summs [expr {$summs + $ms}]
        lappend opslist [expr {$n / ($ms / 1000.0)}]
        incr done $n
    }
    set s [lsort -real $opslist]
    return [list [percentile $s 0.50] [expr {$total / ($summs / 1000.0)}] [llength $opslist]]
}

proc percentile {sorted q} {
    set n [llength $sorted]
    if {$n == 0} {return 0}
    set idx [expr {int(ceil($q * $n)) - 1}]
    if {$idx < 0} {set idx 0}
    if {$idx >= $n} {set idx [expr {$n - 1}]}
    return [lindex $sorted $idx]
}

# Sequential per-op RTT sample (client-observed), microseconds.
proc sample_rtt {fd cmds} {
    set lat {}
    foreach c $cmds {
        set t0 [clock microseconds]
        set r [req $fd {*}$c]
        set t1 [clock microseconds]
        if {[string index $r 0] eq "-"} {error "server error: $r"}
        lappend lat [expr {$t1 - $t0}]
    }
    set s [lsort -real $lat]
    set avg [expr {[tcl::mathop::+ {*}$s] / double([llength $s])}]
    return [list $avg [percentile $s 0.50] [percentile $s 0.99]]
}

# Deterministic vector for member i. "spread" varies every dimension;
# "shared" fixes all but the last dimension (workload B).
proc genvec {i dims g dist} {
    set vals {}
    for {set d 0} {$d < $dims} {incr d} {
        if {$dist eq "shared" && $d < $dims - 1} {
            set v 42.0
        } else {
            set v [expr {(($i * 31 + $d * 101 + $g * 17) % 10000) / 100.0}]
        }
        lappend vals [format %.2f $v]
    }
    return [join $vals "#"]
}

proc member {i} {
    return [format "m%07d" $i]
}

# Delete every other benchmark key so each condition is measured with
# only its own key in memory (constant memory pressure / cache state),
# then purge freed pages back to the OS.
proc isolate_key {fd keep} {
    foreach pat {zvbench:* zbench} {
        set keys [req $fd KEYS $pat]
        foreach k $keys {
            if {$k ne $keep} {
                req $fd DEL $k
            }
        }
    }
    req $fd DEL $keep
    catch {req $fd MEMORY PURGE}
}

proc zvadd_cmd {key dims g dist i} {
    return [list ZVADD $key [genvec $i $dims $g $dist] [member $i]]
}

proc zadd_cmd {key g i} {
    if {$g == 0} {
        set v [expr {(($i * 31) % 10000) / 100.0}]
    } else {
        set v [expr {(($i * 31 + 17) % 10000) / 100.0}]
    }
    return [list ZADD $key [format %.2f $v] [member $i]]
}

set fd [socket 127.0.0.1 $opt(port)]
fconfigure $fd -translation binary -buffering full

set N $opt(n)
set B $opt(batch)
set results {}

puts "ZV PoC benchmark: N=$N batch=$B dims=($opt(dims)) dists=($opt(dists)) port=$opt(port)"

foreach dist $opt(dists) {
    foreach dims $opt(dims) {
        set key "zvbench:$dist:d$dims"
        isolate_key $fd $key

        # --- insert ---
        lassign [run_batches $fd $N $B [list zvadd_cmd $key $dims 0 $dist]] med tot nb
        puts [format "%-6s dims=%-3s ZVADD-insert   median %10.0f ops/sec (total %10.0f, %d batches)" \
            $dist $dims $med $tot $nb]
        lappend results [list $dims $dist ZVADD-insert $med "" ""]

        # --- update (same members, new vectors) ---
        lassign [run_batches $fd $N $B [list zvadd_cmd $key $dims 1 $dist]] med tot nb
        puts [format "%-6s dims=%-3s ZVADD-update   median %10.0f ops/sec (total %10.0f, %d batches)" \
            $dist $dims $med $tot $nb]
        lappend results [list $dims $dist ZVADD-update $med "" ""]

        # --- rank / score sampling ---
        set S $opt(samples)
        set rankcmds {}
        set scorecmds {}
        for {set s 0} {$s < $S} {incr s} {
            set m [member [expr {($s * 7919) % $N}]]
            lappend rankcmds [list ZVRANK $key $m]
            lappend scorecmds [list ZVSCORE $key $m]
        }
        foreach {name cmds} [list ZVRANK $rankcmds ZVSCORE $scorecmds] {
            set ms [pipeline $fd $cmds]
            set ops [expr {$S / ($ms / 1000.0)}]
            lassign [sample_rtt $fd [lrange $cmds 0 [expr {$opt(rtts) - 1}]]] avg p50 p99
            puts [format "%-6s dims=%-3s %-12s %10.0f ops/sec  rtt avg=%.1fus p50=%.1fus p99=%.1fus" \
                $dist $dims $name $ops $avg $p50 $p99]
            lappend results [list $dims $dist $name $ops $p50 $p99]
        }

        # --- range ---
        foreach {name extra} {ZVRANGE {} ZVRANGE-WITHSCORES {WITHSCORES}} {
            set cmds {}
            for {set s 0} {$s < 100} {incr s} {
                if {$extra eq ""} {
                    lappend cmds [list ZVRANGE $key 0 99]
                } else {
                    lappend cmds [list ZVRANGE $key 0 99 WITHSCORES]
                }
            }
            set ms [pipeline $fd $cmds]
            set ops [expr {100 / ($ms / 1000.0)}]
            puts [format "%-6s dims=%-3s %-18s %10.0f ops/sec" $dist $dims $name $ops]
            lappend results [list $dims $dist $name $ops "" ""]
        }

        set mem [req $fd MEMORY USAGE $key]
        puts "$dist dims=$dims MEMORY USAGE $key = $mem bytes"
        lappend results [list $dims $dist MEMORY-USAGE $mem "" ""]
    }
}

# --- ZSET baseline (dimension 1 equivalent) ---
isolate_key $fd zbench
lassign [run_batches $fd $N $B [list zadd_cmd zbench 0]] med tot nb
puts [format "ZSET      ZADD-insert    median %10.0f ops/sec (total %10.0f, %d batches)" $med $tot $nb]
lappend results [list 1 spread ZSET-ZADD-insert $med "" ""]
lassign [run_batches $fd $N $B [list zadd_cmd zbench 1]] med tot nb
puts [format "ZSET      ZADD-update    median %10.0f ops/sec (total %10.0f, %d batches)" $med $tot $nb]
lappend results [list 1 spread ZSET-ZADD-update $med "" ""]
set S $opt(samples)
set rankcmds {}
set scorecmds {}
for {set s 0} {$s < $S} {incr s} {
    set m [member [expr {($s * 7919) % $N}]]
    lappend rankcmds [list ZRANK zbench $m]
    lappend scorecmds [list ZSCORE zbench $m]
}
foreach {name cmds} [list ZRANK $rankcmds ZSCORE $scorecmds] {
    set ms [pipeline $fd $cmds]
    lassign [sample_rtt $fd [lrange $cmds 0 [expr {$opt(rtts) - 1}]]] avg p50 p99
    puts [format "ZSET      %-12s %10.0f ops/sec  rtt avg=%.1fus p50=%.1fus p99=%.1fus" \
        $name [expr {$S / ($ms / 1000.0)}] $avg $p50 $p99]
    lappend results [list 1 spread ZSET-$name [expr {$S / ($ms / 1000.0)}] $p50 $p99]
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
    set ms [pipeline $fd $cmds]
    puts [format "ZSET      %-18s %10.0f ops/sec" $name [expr {100 / ($ms / 1000.0)}]]
    lappend results [list 1 spread ZSET-$name [expr {100 / ($ms / 1000.0)}] "" ""]
}
puts "ZSET      MEMORY USAGE zbench = [req $fd MEMORY USAGE zbench] bytes"

close $fd

if {$opt(out) ne ""} {
    set f [open $opt(out) w]
    puts $f "dims\tdist\top\tops_per_sec\tp50_us\tp99_us"
    foreach r $results {
        puts $f [join $r "\t"]
    }
    close $f
    puts "wrote $opt(out)"
}
