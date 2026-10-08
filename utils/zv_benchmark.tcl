#!/usr/bin/env tclsh
# ZVSET PoC benchmark.
#
# Measures, for dimensions 1/2/4/8/16 (configurable):
#   ZVADD new insert, ZVADD update, ZVRANK, ZVSCORE,
#   ZVRANGE 0 99, ZVRANGE 0 99 WITHSCORES
# plus a ZSET baseline at dimension 1.
#
# Reports ops/sec and client-observed avg/p50/p99 latency. The p50/p99
# numbers below are loopback RTT samples (server + stack), useful for
# relative comparison across dimensions, not absolute server latency.
#
# Usage:
#   tclsh utils/zv_benchmark.tcl [--port 6379] [--n 1000000] \
#       [--dims "1 2 4 8 16"] [--samples 200] [--out results.tsv]
#
# SPDX-License-Identifier: BSD-3-Clause

package require Tcl 8.6

array set opt {port 6379 n 1000000 dims {1 2 4 8 16} samples 200 out ""}
for {set i 0} {$i < $argc} {incr i} {
    set a [lindex $argv $i]
    switch -- $a {
        --port {incr i; set opt(port) [lindex $argv $i]}
        --n {incr i; set opt(n) [lindex $argv $i]}
        --dims {incr i; set opt(dims) [lindex $argv $i]}
        --samples {incr i; set opt(samples) [lindex $argv $i]}
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

# Pipeline a list of argv-lists; returns elapsed ms. Replies are read
# and discarded (validated as non-error).
proc pipeline {fd cmds} {
    set buf ""
    foreach c $cmds {
        append buf [pack {*}$c]
    }
    set t0 [clock microseconds]
    puts -nonewline $fd $buf
    flush $fd
    set n [llength $cmds]
    for {set i 0} {$i < $n} {incr i} {
        set r [readReply $fd]
        if {[string index $r 0] eq "-"} {
            error "server error: $r"
        }
    }
    set t1 [clock microseconds]
    return [expr {($t1 - $t0) / 1000.0}]
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

# Deterministic vector for member i, dimension k, generation g.
proc genvec {i k dims g} {
    set vals {}
    for {set d 0} {$d < $dims} {incr d} {
        set v [expr {(($i * 31 + $d * 101 + $g * 17) % 10000) / 100.0}]
        lappend vals [format %.2f $v]
    }
    return [join $vals "#"]
}

proc member {i} {
    return [format "m%07d" $i]
}

set fd [socket 127.0.0.1 $opt(port)]
fconfigure $fd -translation binary -buffering full
req $fd HELLO 3 >/dev/null

set N $opt(n)
set results {}

puts "ZV PoC benchmark: N=$N dims=($opt(dims)) port=$opt(port)"

foreach dims $opt(dims) {
    set key "zvbench:d$dims"
    req $fd DEL $key

    # --- insert ---
    set cmds {}
    for {set i 0} {$i < $N} {incr i} {
        lappend cmds [list ZVADD $key [genvec $i 0 $dims 0] [member $i]]
    }
    set ms [pipeline $fd $cmds]
    set ops [expr {$N / ($ms / 1000.0)}]
    puts [format "dims=%-3s ZVADD-insert  %10.0f ops/sec" $dims $ops]
    lappend results [list $dims ZVADD-insert $ops "" "" ""]

    # --- update (same members, new vectors) ---
    set cmds {}
    for {set i 0} {$i < $N} {incr i} {
        lappend cmds [list ZVADD $key [genvec $i 0 $dims 1] [member $i]]
    }
    set ms [pipeline $fd $cmds]
    set ops [expr {$N / ($ms / 1000.0)}]
    puts [format "dims=%-3s ZVADD-update  %10.0f ops/sec" $dims $ops]
    lappend results [list $dims ZVADD-update $ops "" "" ""]

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
        lassign [sample_rtt $fd [lrange $cmds 0 199]] avg p50 p99
        puts [format "dims=%-3s %-12s %10.0f ops/sec  rtt avg=%.1fus p50=%.1fus p99=%.1fus" \
            $dims $name $ops $avg $p50 $p99]
        lappend results [list $dims $name $ops $p50 $p99]
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
        puts [format "dims=%-3s %-18s %10.0f ops/sec" $dims $name $ops]
        lappend results [list $dims $name $ops "" ""]
    }

    set mem [req $fd MEMORY USAGE $key]
    puts "dims=$dims MEMORY USAGE $key = $mem bytes"
    lappend results [list $dims MEMORY-USAGE $mem "" ""]
}

# --- ZSET baseline (dimension 1 equivalent) ---
req $fd DEL zbench
set cmds {}
for {set i 0} {$i < $N} {incr i} {
    set v [expr {(($i * 31) % 10000) / 100.0}]
    lappend cmds [list ZADD zbench [format %.2f $v] [member $i]]
}
set ms [pipeline $fd $cmds]
puts [format "ZSET      ZADD-insert   %10.0f ops/sec" [expr {$N / ($ms / 1000.0)}]]
lappend results [list 1 ZSET-ZADD-insert [expr {$N / ($ms / 1000.0)}] "" ""]
set cmds {}
for {set i 0} {$i < $N} {incr i} {
    set v [expr {(($i * 31 + 17) % 10000) / 100.0}]
    lappend cmds [list ZADD zbench [format %.2f $v] [member $i]]
}
set ms [pipeline $fd $cmds]
puts [format "ZSET      ZADD-update   %10.0f ops/sec" [expr {$N / ($ms / 1000.0)}]]
lappend results [list 1 ZSET-ZADD-update [expr {$N / ($ms / 1000.0)}] "" ""]
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
    lassign [sample_rtt $fd [lrange $cmds 0 199]] avg p50 p99
    puts [format "ZSET      %-12s %10.0f ops/sec  rtt avg=%.1fus p50=%.1fus p99=%.1fus" \
        $name [expr {$S / ($ms / 1000.0)}] $avg $p50 $p99]
    lappend results [list 1 ZSET-$name [expr {$S / ($ms / 1000.0)}] $p50 $p99]
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
    lappend results [list 1 ZSET-$name [expr {100 / ($ms / 1000.0)}] "" ""]
}
puts "ZSET      MEMORY USAGE zbench = [req $fd MEMORY USAGE zbench] bytes"

close $fd

if {$opt(out) ne ""} {
    set f [open $opt(out) w]
    puts $f "dims\top\tops_per_sec\tp50_us\tp99_us"
    foreach r $results {
        puts $f [join $r "\t"]
    }
    close $f
    puts "wrote $opt(out)"
}
