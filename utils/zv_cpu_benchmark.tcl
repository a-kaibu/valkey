#!/usr/bin/env tclsh
# Measure server execution time separately from client RESP parsing and RTT.
# Run against a dedicated, persistence-disabled server with no other clients.
# Usage: tclsh utils/zv_cpu_benchmark.tcl --port 6379 --n 100000 --dims 16
# Use --suite random for sampling/cardinality, or --suite pop for destructive pops.
# SPDX-License-Identifier: BSD-3-Clause

source [file join [file dirname [info script]] zv_bench_lib.tcl]
array set opt {port 6379 n 100000 dims 16 repeats 7 suite all}
for {set i 0} {$i < $argc} {incr i} {
    set arg [lindex $argv $i]
    set name [string range $arg 2 end]
    if {![string match --* $arg] || ![info exists opt($name)] || $i + 1 >= $argc} {
        error "invalid argument: $arg"
    }
    incr i
    set opt($name) [lindex $argv $i]
}
foreach name {port n dims repeats} {
    if {![string is integer -strict $opt($name)] || $opt($name) < 1} {
        error "$name must be a positive integer"
    }
}
if {$opt(dims) > 255} {error "dims must be <= 255"}
if {$opt(suite) ni {all random pop}} {error "suite must be all, random or pop"}

proc command_usec {fd command} {
    set info [zvbench::req $fd INFO commandstats]
    if {[regexp "cmdstat_${command}:calls=\[0-9\]+,usec=(\[0-9\]+)," $info -> usec]} {
        return $usec
    }
    return 0
}

proc prepare_measurement {fd cmd} {
    if {![llength $cmd]} return
    set result [zvbench::req $fd {*}$cmd]
    if {$result ne "1"} {error "measurement preparation failed: $result"}
}

proc measure {fd label cmd iterations repeats {prepare {}}} {
    set command [string tolower [lindex $cmd 0]]
    # Encode once outside the timer; only transport and replies are timed.
    set payload [string repeat [zvbench::pack {*}$cmd] $iterations]
    prepare_measurement $fd $prepare
    set result [zvbench::req $fd {*}$cmd]
    if {[string index $result 0] eq "-"} {error $result}
    set cpu {}
    set wall {}
    for {set rep 0} {$rep < $repeats} {incr rep} {
        # Destructive commands start from the same contents, outside the timer.
        prepare_measurement $fd $prepare
        set before [command_usec $fd $command]
        set start [clock microseconds]
        puts -nonewline $fd $payload
        flush $fd
        for {set i 0} {$i < $iterations} {incr i} {
            set result [zvbench::readReply $fd]
            if {[string index $result 0] eq "-"} {error $result}
        }
        lappend wall [expr {([clock microseconds] - $start) / double($iterations)}]
        lappend cpu [expr {([command_usec $fd $command] - $before) / double($iterations)}]
    }
    set sorted [lsort -real $cpu]
    puts [join [list $label [zvbench::percentile $sorted 0.5] \
        [lindex $sorted 0] [lindex $sorted end] \
        [zvbench::percentile [lsort -real $wall] 0.5] $repeats $iterations] \t]
}

set fd [zvbench::connect $opt(port)]
foreach line [split [zvbench::req $fd INFO server] \n] {
    set line [string trim $line]
    set name [lindex [split $line :] 0]
    if {$name in {redis_version redis_git_sha1 redis_git_dirty redis_build_id arch_bits}} {
        puts "# $line"
    }
}
set prefix zv-cpu
set keys {}
for {set k 0} {$k < 4} {incr k} {lappend keys $prefix:$k}
lappend keys $prefix:dst
# Only remove keys owned by this benchmark.
zvbench::req $fd DEL {*}$keys
set inputs [expr {$opt(suite) eq "all" ? 4 : 1}]
set dist [expr {$opt(suite) eq "pop" ? "spread" : "rand_uniform"}]
for {set k 0} {$k < $inputs} {incr k} {
    zvbench::run_batches $fd $opt(n) 1000 \
        [list zvbench::zvadd_cmd $prefix:$k $opt(dims) $k $dist]
}
puts "# N=$opt(n) dims=$opt(dims) suite=$opt(suite), dedicated server required"
puts "case\tserver_us\tserver_min_us\tserver_max_us\twall_us\trepeats\titerations"
if {$opt(suite) eq "pop"} {
    set prepare [list COPY $prefix:0 $prefix:dst REPLACE]
    foreach count {1 100} {
        if {$count > $opt(n)} continue
        set iterations [expr {min(1000, $opt(n) / $count)}]
        foreach where {MIN MAX} {
            measure $fd pop-$where-$count [list ZVPOP$where $prefix:dst $count] \
                $iterations $opt(repeats) $prepare
        }
    }
    zvbench::req $fd DEL {*}$keys
    close $fd
    exit
}
foreach pct {1 10 25 50 90 100} {
    set count [expr {max(1, $opt(n) * $pct / 100)}]
    measure $fd random-$pct-pct [list ZVRANDMEMBER $prefix:0 $count] 2 $opt(repeats)
}
measure $fd random-sparse [list ZVRANDMEMBER $prefix:0 10] 100 $opt(repeats)
measure $fd random-duplicates [list ZVRANDMEMBER $prefix:0 -100] 100 $opt(repeats)
measure $fd random-full-scores [list ZVRANDMEMBER $prefix:0 $opt(n) WITHSCORES] 2 $opt(repeats)
measure $fd intercard-single [list ZVINTERCARD 1 $prefix:0] 100 $opt(repeats)
if {$opt(suite) eq "random"} {
    zvbench::req $fd DEL {*}$keys
    close $fd
    exit
}
foreach {label args} {
    query-all {}
    query-10pct {FILTER 0 45 55}
    query-1pct {FILTER 0 49.5 50.5}
    query-0.01pct {FILTER 0 50 50.01}
    query-empty {FILTER 0 101 102}
    query-limit10 {FILTER 0 45 55 LIMIT 0 10}
    query-limit0 {FILTER 0 45 55 LIMIT 0 0}
    query-offset {LIMIT 20000 10}
    query-filter2 {FILTER 0 45 55 FILTER 1 45 55}
} {
    # Query percentages name the interval width within [0,100], not the
    # measured result fraction; inclusive bounds cover quantized endpoints.
    if {$label eq "query-filter2" && $opt(dims) < 2} continue
    # Avoid enormous pipelines when returning the full dataset.
    set iterations [expr {$label eq "query-all" ? 2 : 100}]
    measure $fd $label [list ZVQUERY $prefix:0 {*}$args] $iterations $opt(repeats)
}
measure $fd query-last [list ZVQUERY $prefix:0 FILTER [expr {$opt(dims)-1}] 49.5 50.5] 100 $opt(repeats)
measure $fd range-scores [list ZVRANGE $prefix:0 0 99 WITHSCORES] 100 $opt(repeats)
set lo [join [lrepeat $opt(dims) 40] #]
set hi [join [lrepeat $opt(dims) 60] #]
measure $fd rangebyscore-scores [list ZVRANGEBYSCORE $prefix:0 $lo $hi WITHSCORES LIMIT 0 100] 100 $opt(repeats)
measure $fd count [list ZVCOUNT $prefix:0 $lo $hi] 1000 $opt(repeats)
measure $fd rank [list ZVRANK $prefix:0 [zvbench::member 0]] 1000 $opt(repeats)
measure $fd score [list ZVSCORE $prefix:0 [zvbench::member 0]] 1000 $opt(repeats)
set members {}
for {set m 0} {$m < 10} {incr m} {lappend members [zvbench::member $m]}
measure $fd mscore-10 [list ZVMSCORE $prefix:0 {*}$members] 1000 $opt(repeats)
set delta [join [lrepeat $opt(dims) 0] #]
measure $fd incrby-zero [list ZVINCRBY $prefix:0 $delta [zvbench::member 0]] 1000 $opt(repeats)
set vec [zvbench::genvec 0 $opt(dims) 0 rand_uniform]
measure $fd update-noop [list ZVADD $prefix:0 $vec [zvbench::member 0]] 1000 $opt(repeats)
measure $fd update-gt-noop [list ZVADD $prefix:0 GT $vec [zvbench::member 0]] 1000 $opt(repeats)
measure $fd update-lt-noop [list ZVADD $prefix:0 LT $vec [zvbench::member 0]] 1000 $opt(repeats)
set lower [join [lrepeat $opt(dims) -1] #]
measure $fd update-gt-reject [list ZVADD $prefix:0 GT $lower [zvbench::member 0]] 1000 $opt(repeats)
foreach aggregate {SUM MIN MAX} {
    measure $fd unionstore-$aggregate \
        [list ZVUNIONSTORE $prefix:dst 4 {*}[lrange $keys 0 3] AGGREGATE $aggregate] 5 $opt(repeats)
}
measure $fd interstore [list ZVINTERSTORE $prefix:dst 4 {*}[lrange $keys 0 3]] 5 $opt(repeats)
measure $fd intercard [list ZVINTERCARD 4 {*}[lrange $keys 0 3]] 5 $opt(repeats)
measure $fd diffstore [list ZVDIFFSTORE $prefix:dst 2 {*}[lrange $keys 0 1]] 5 $opt(repeats)
# A small key (64 members) exercises the compact encoding.
set small $prefix:small
zvbench::req $fd DEL $small
for {set m 0} {$m < 64} {incr m} {
    zvbench::req $fd ZVADD $small [zvbench::genvec $m $opt(dims) 0 rand_uniform] [zvbench::member $m]
}
measure $fd small-score [list ZVSCORE $small [zvbench::member 63]] 1000 $opt(repeats)
measure $fd small-rank [list ZVRANK $small [zvbench::member 63]] 1000 $opt(repeats)
measure $fd small-range-scores [list ZVRANGE $small 0 -1 WITHSCORES] 100 $opt(repeats)
measure $fd small-count [list ZVCOUNT $small $lo $hi] 1000 $opt(repeats)
set one [join [lrepeat $opt(dims) 1] #]
measure $fd small-incrby [list ZVINCRBY $small $one [zvbench::member 63]] 1000 $opt(repeats)
zvbench::req $fd DEL {*}$keys $small
close $fd
