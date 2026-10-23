#!/usr/bin/env tclsh
# Server execution time of existing ZSET commands (listpack and btree
# encodings) next to their one-dimensional ZV equivalents. Used by the
# zset-ab CI job to check that the ZV changes do not slow ZSET down, and to
# compare ZV with ZSET on the same server.
# Usage: tclsh utils/zset_compare_benchmark.tcl --port 6379 [--n 30000] [--small 64] [--repeats 5] [--kinds "z zv"]
# Run against a dedicated, persistence-disabled server with no other clients.
# Output: TSV "case<TAB>server_us<TAB>min<TAB>max<TAB>wall_us<TAB>repeats<TAB>iterations".
# A server without the ZV commands (e.g. the base branch) skips zv-* cases.
# SPDX-License-Identifier: BSD-3-Clause

source [file join [file dirname [info script]] zv_bench_lib.tcl]
array set opt {port 6379 n 30000 small 64 repeats 5 kinds {z zv}}
for {set i 0} {$i < $argc} {incr i} {
    set arg [lindex $argv $i]
    set name [string range $arg 2 end]
    if {![string match --* $arg] || ![info exists opt($name)] || $i + 1 >= $argc} {
        error "invalid argument: $arg"
    }
    incr i
    set opt($name) [lindex $argv $i]
}

proc command_usec {fd command} {
    set info [zvbench::req $fd INFO commandstats]
    if {[regexp "cmdstat_${command}:calls=\[0-9\]+,usec=(\[0-9\]+)," $info -> usec]} {
        return $usec
    }
    return 0
}

proc check {result} {
    if {[string index $result 0] eq "-"} {error $result}
    return $result
}

# Time a payload of commands (all for the same command name); prepare runs
# outside the timer before every repeat.
proc measure_payload {fd label command cmds repeats {prepare {}}} {
    set payload {}
    foreach cmd $cmds {append payload [zvbench::pack {*}$cmd]}
    set iterations [llength $cmds]
    set cpu {}
    set wall {}
    for {set rep 0} {$rep < $repeats} {incr rep} {
        foreach p $prepare {check [zvbench::req $fd {*}$p]}
        set before [command_usec $fd $command]
        set start [clock microseconds]
        puts -nonewline $fd $payload
        flush $fd
        for {set i 0} {$i < $iterations} {incr i} {check [zvbench::readReply $fd]}
        lappend wall [expr {([clock microseconds] - $start) / double($iterations)}]
        lappend cpu [expr {([command_usec $fd $command] - $before) / double($iterations)}]
    }
    set sorted [lsort -real $cpu]
    # Under valgrind --tool=callgrind, ZCMP_CALLGRIND_PID=<pid> dumps
    # the profile after every case, for instruction counts per case.
    # --max-invoke-ms=0: wait for the server to wake up (serverCron) rather
    # than ptrace it, which CI runners do not allow.
    if {[info exists ::env(ZCMP_CALLGRIND_PID)]} {
        exec vgdb --pid=$::env(ZCMP_CALLGRIND_PID) --max-invoke-ms=0 dump $label >/dev/null 2>/dev/null
    }
    puts [join [list $label [zvbench::percentile $sorted 0.5] [lindex $sorted 0] [lindex $sorted end] \
        [zvbench::percentile [lsort -real $wall] 0.5] $repeats $iterations] \t]
    flush stdout
}

proc measure {fd label cmd iterations repeats {prepare {}}} {
    measure_payload $fd $label [string tolower [lindex $cmd 0]] [lrepeat $iterations $cmd] $repeats $prepare
}

proc member {i} {return [format "m%07d" $i]}
proc score {i} {return [expr {($i * 7919) % 100003}]}

set fd [zvbench::connect $opt(port)]
set has_zv [expr {[string index [zvbench::req $fd ZVCARD zcmp:probe] 0] ne "-"}]
zvbench::req $fd CONFIG SET zset-max-listpack-entries 128
zvbench::req $fd CONFIG SET zset-max-listpack-value 64

set n $opt(n)
set small $opt(small)
set r $opt(repeats)
puts "# N=$n small=$small has_zv=$has_zv"
puts "case\tserver_us\tserver_min_us\tserver_max_us\twall_us\trepeats\titerations"

# kind: z (ZSET) or zv (ZV, 1 dimension). size: small (listpack/compact)
# or large (btree).
foreach kind $opt(kinds) {
    if {$kind eq "zv" && !$has_zv} continue
    set P [string toupper $kind]
    foreach size {small large} {
        set count [expr {$size eq "small" ? $small : $n}]
        set key "zcmp:$kind:$size"
        set src "zcmp:$kind:$size:src"
        set dst "zcmp:$kind:$size:dst"
        set other "zcmp:$kind:$size:other"
        zvbench::req $fd DEL $key $src $dst $other
        # Insert: fresh members into an emptied key.
        set adds {}
        for {set i 0} {$i < $count} {incr i} {lappend adds [list ${P}ADD $key [score $i] [member $i]]}
        set label "$kind-$size"
        measure_payload $fd $label-add-new [string tolower ${P}add] $adds $r [list [list DEL $key]]
        # Snapshot for destructive cases and a half-overlapping set for setops.
        check [zvbench::req $fd COPY $key $src]
        set others {}
        for {set i [expr {$count / 2}]} {$i < $count + $count / 2} {incr i} {
            lappend others [list ${P}ADD $other [score $i] [member $i]]
        }
        zvbench::pipeline $fd $others
        set it 1000
        set mid [member [expr {$count / 2}]]
        set lo [score 1000]
        set hi [expr {$lo + 1000}]
        measure $fd $label-add-update [list ${P}ADD $key [score 7] $mid] $it $r
        measure $fd $label-incrby [list ${P}INCRBY $key 1 $mid] $it $r
        measure $fd $label-score [list ${P}SCORE $key $mid] $it $r
        measure $fd $label-mscore-10 [list ${P}MSCORE $key {*}[lmap i {0 1 2 3 4 5 6 7 8 9} {member [expr {$i * $count / 10}]}]] $it $r
        measure $fd $label-rank [list ${P}RANK $key $mid] $it $r
        measure $fd $label-card [list ${P}CARD $key] $it $r
        measure $fd $label-range-10 [list ${P}RANGE $key 0 9 WITHSCORES] $it $r
        measure $fd $label-range-100 [list ${P}RANGE $key 0 99 WITHSCORES] 200 $r
        measure $fd $label-revrange-10 [list ${P}REVRANGE $key 0 9] $it $r
        measure $fd $label-rangebyscore [list ${P}RANGEBYSCORE $key $lo $hi LIMIT 0 10] $it $r
        measure $fd $label-count [list ${P}COUNT $key $lo $hi] $it $r
        measure $fd $label-randmember [list ${P}RANDMEMBER $key 10] $it $r
        # Destructive cases restart from the snapshot every repeat.
        set restore [list [list COPY $src $key REPLACE]]
        set rems {}
        for {set i 0} {$i < min($count, 1000)} {incr i} {lappend rems [list ${P}REM $key [member $i]]}
        measure_payload $fd $label-rem [string tolower ${P}rem] $rems $r $restore
        measure $fd $label-popmin [list ${P}POPMIN $key] [expr {min($count, 1000)}] $r $restore
        measure $fd $label-bzpopmin [list B${P}POPMIN $key 0] [expr {min($count, 1000)}] $r $restore
        measure $fd $label-remrangebyrank [list ${P}REMRANGEBYRANK $key 0 0] [expr {min($count, 1000)}] $r $restore
        # Set operations over two keys sharing half their members.
        set sit [expr {$size eq "small" ? 200 : 3}]
        measure $fd $label-unionstore [list ${P}UNIONSTORE $dst 2 $src $other] $sit $r
        measure $fd $label-interstore [list ${P}INTERSTORE $dst 2 $src $other] $sit $r
        measure $fd $label-diffstore [list ${P}DIFFSTORE $dst 2 $src $other] $sit $r
        measure $fd $label-intercard [list ${P}INTERCARD 2 $src $other] $sit $r
        measure $fd $label-rangestore [list ${P}RANGESTORE $dst $src 0 99] $sit $r
        zvbench::req $fd DEL $key $src $dst $other
    }
}
close $fd
