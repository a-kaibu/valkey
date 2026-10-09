#!/usr/bin/env tclsh
# Shared harness for ZVSET PoC benchmarks (sourced, not executed).
#
# Provides: RESP2 client, batched pipelines with median stats, RTT
# sampling, deterministic score distributions, key isolation, TSV +
# JSONL result writers.
#
# Distributions (genvec i dims g dist):
#   spread       every dimension varies; comparisons usually settle early.
#   shared       all but the last dimension fixed (long common prefixes).
#   all_equal    constant vector; order decided by member bytes only.
#   lowcard      few distinct values per dimension; massive ties.
#   rand_uniform independent uniform per dimension (deterministic LCG).
#   sequential   monotonically increasing with i (ordered inserts).
#   revseq       monotonically decreasing with i (reverse inserts).
#   zipf         skewed value frequencies (approx, deterministic).
#
# SPDX-License-Identifier: BSD-3-Clause

package require Tcl 8.6

namespace eval zvbench {
    variable results {}
    variable commit ""

    proc lcg {seed} {
        return [expr {($seed * 1103515245 + 12345) & 0x7fffffff}]
    }

    # Stronger deterministic uniform in [0,1): splitmix-style finalizer
    # over (i,d,g). (Raw LCG low bits are too weak for % 10000.)
    proc randu {i d g} {
        set x [hash3 $i $d $g]
        set x [expr {($x ^ ($x >> 13)) & 0x7fffffff}]
        set x [expr {($x * 1274126177) & 0x7fffffff}]
        set x [expr {($x ^ ($x >> 16)) & 0x7fffffff}]
        return [expr {($x % 10000) / 100.0}]
    }

    proc hash3 {a b c} {
        return [expr {(($a * 374761393 + $b * 668265263 + $c * 974634211) & 0x7fffffff)}]
    }

    proc pack {args} {
        set out "*[llength $args]\r\n"
        foreach a $args {
            append out "\$[string length $a]\r\n$a\r\n"
        }
        return $out
    }

    # Minimal RESP2 client parser.
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

    proc connect {port} {
        set fd [socket 127.0.0.1 $port]
        fconfigure $fd -translation binary -buffering full
        return $fd
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

    proc percentile {sorted q} {
        set n [llength $sorted]
        if {$n == 0} {return 0}
        set idx [expr {int(ceil($q * $n)) - 1}]
        if {$idx < 0} {set idx 0}
        if {$idx >= $n} {set idx [expr {$n - 1}]}
        return [lindex $sorted $idx]
    }

    # Run $total commands built by "$buildCmd $index" in $batch sized
    # pipelines. Returns {median_batch_ops total_ops nbatches}.
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

    # Sequential per-op RTT sample (client-observed), microseconds.
    # Returns {avg p50 p99}.
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

    proc member {i} {
        return [format "m%07d" $i]
    }

    # Deterministic vector for member i, generation g.
    proc genvec {i dims g dist} {
        set vals {}
        for {set d 0} {$d < $dims} {incr d} {
            switch -- $dist {
                shared {
                    if {$d < $dims - 1} {
                        set v 42.0
                    } else {
                        set v [expr {(($i * 31 + $g * 17) % 10000) / 100.0}]
                    }
                }
                all_equal {
                    set v 1.0
                }
                lowcard {
                    set v [expr {(($i * 31 + $d * 101 + $g * 17) % 5)}]
                    set v [expr {$v + 0.0}]
                }
                rand_uniform {
                    set v [randu $i $d $g]
                }
                sequential {
                    set v [expr {($i + $g * 3) / 100.0 + $d * 0.001}]
                }
                revseq {
                    set v [expr {(1000000 - $i + $g * 3) / 100.0 - $d * 0.001}]
                }
                zipf {
                    set v [expr {(($i * $i + $d * 13 + $g * 7) % 1000) / 100.0}]
                }
                default {
                    set v [expr {(($i * 31 + $d * 101 + $g * 17) % 10000) / 100.0}]
                }
            }
            lappend vals [format %.2f $v]
        }
        return [join $vals "#"]
    }

    proc zvadd_cmd {key dims g dist i} {
        return [list ZVADD $key [genvec $i $dims $g $dist] [member $i]]
    }

    # Delete every other benchmark key so each condition is measured with
    # only its own key in memory, then purge freed pages back to the OS.
    proc isolate_key {fd keep {patterns {zvbench:* zbench zvrange:* zvpop:* zvsetops:* zvquery:*}}} {
        foreach pat $patterns {
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

    proc mem_usage {fd key} {
        return [req $fd MEMORY USAGE $key]
    }

    proc used_memory {fd} {
        set info [req $fd INFO memory]
        foreach line [split $info "\n"] {
            if {[string match "used_memory:*" $line]} {
                return [string range $line [string length "used_memory:"] end]
            }
        }
        return -1
    }

    proc git_sha {} {
        variable commit
        if {$commit ne ""} {return $commit}
        if {[catch {exec git rev-parse --short HEAD} sha]} {
            set commit "unknown"
        } else {
            set commit [string trim $sha]
        }
        return $commit
    }

    proc reset_results {} {
        variable results
        set results {}
    }

    # Rotated condition order for repeat r: spreads order effects
    # (cache warmth, allocator state) evenly across conditions.
    proc rotated {conds r} {
        set n [llength $conds]
        if {$n == 0} {return {}}
        set o [expr {$r % $n}]
        if {$o == 0} {return $conds}
        return [concat [lrange $conds $o end] [lrange $conds 0 [expr {$o - 1}]]]
    }

    # Collapse per-repeat rows into median rows. Grouped by
    # {dims dist op}; ops/p50/p99 take medians, min/max go to extra
    # as rep_min/rep_max along with the repeat count. Preserves the
    # first-seen condition order.
    proc collapse_results {} {
        variable results
        array set g {}
        set order {}
        foreach r $results {
            lassign $r dims dist op ops p50 p99 extra
            set k "$dims\t$dist\t$op"
            if {![info exists g($k,n)]} {
                lappend order $k
                set g($k,n) 0
                set g($k,ops) {}
                set g($k,p50) {}
                set g($k,p99) {}
                set g($k,extra) $extra
                set g($k,dims) $dims
                set g($k,dist) $dist
                set g($k,op) $op
            }
            incr g($k,n)
            lappend g($k,ops) $ops
            if {$p50 ne ""} {lappend g($k,p50) $p50}
            if {$p99 ne ""} {lappend g($k,p99) $p99}
        }
        set out {}
        foreach k $order {
            set ops [lsort -real $g($k,ops)]
            set med [percentile $ops 0.50]
            set extra $g($k,extra)
            dict set extra rep_min [lindex $ops 0]
            dict set extra rep_max [lindex $ops end]
            dict set extra reps $g($k,n)
            set p50 ""
            set p99 ""
            if {[llength $g($k,p50)] > 0} {
                set p50 [percentile [lsort -real $g($k,p50)] 0.50]
            }
            if {[llength $g($k,p99)] > 0} {
                set p99 [percentile [lsort -real $g($k,p99)] 0.50]
            }
            lappend out [list $g($k,dims) $g($k,dist) $g($k,op) $med $p50 $p99 $extra]
        }
        array unset g
        set results $out
    }

    # Print the collapsed median table (call after collapse_results).
    proc print_medians {} {
        variable results
        puts "--- medians over repeats (min..max) ---"
        foreach r $results {
            lassign $r dims dist op ops p50 p99 extra
            set rng ""
            if {[dict exists $extra rep_min]} {
                set rng [format " (min %.0f, max %.0f, n=%s)" \
                    [dict get $extra rep_min] [dict get $extra rep_max] [dict get $extra reps]]
            }
            if {$op eq "MEMORY-USAGE"} {
                puts [format "%-8s dims=%-3s %-22s %10.0f bytes%s" $dist $dims $op $ops $rng]
            } elseif {[string match "*-us" $op]} {
                puts [format "%-8s dims=%-3s %-22s %10.1f us%s" $dist $dims $op $ops $rng]
            } else {
                puts [format "%-8s dims=%-3s %-22s %10.0f ops/sec%s" $dist $dims $op $ops $rng]
            }
        }
    }

    # Row: {dims dist op ops p50 p99 extra-dict}.
    proc row {dims dist op ops p50 p99 {extra {}}} {
        variable results
        lappend results [list $dims $dist $op $ops $p50 $p99 $extra]
    }

    proc write_tsv {path} {
        variable results
        set f [open $path w]
        puts $f "dims\tdist\top\tops_per_sec\tp50_us\tp99_us\textra"
        foreach r $results {
            puts $f [join $r "\t"]
        }
        close $f
        puts "wrote $path"
    }

    # JSONL sidecar per benchmark-design: commit, command, operation,
    # dimensions, members, distribution, pipeline, connections, ops,
    # latencies, memory.
    proc write_jsonl {path members pipeline conns} {
        variable results
        set sha [git_sha]
        set f [open $path w]
        foreach r $results {
            lassign $r dims dist op ops p50 p99 extra
            set mem ""
            if {[dict exists $extra memory_bytes]} {
                set mem [dict get $extra memory_bytes]
            }
            set obj [dict create commit $sha command [lindex [split $op -] 0] \
                operation $op dimensions $dims members $members \
                distribution $dist pipeline $pipeline connections $conns \
                ops_per_sec $ops p50_us $p50 p99_us $p99 memory_bytes $mem]
            if {$extra ne ""} {
                dict for {k v} $extra {
                    if {$k ne "memory_bytes"} {dict set obj $k $v}
                }
            }
            puts $f "{"
            set parts {}
            dict for {k v} $obj {
                if {$v eq ""} continue
                if {[string is double -strict $v]} {
                    lappend parts "  \"$k\": $v"
                } else {
                    lappend parts "  \"$k\": \"$v\""
                }
            }
            puts $f [join $parts ",\n"]
            puts $f "}"
        }
        close $f
        puts "wrote $path"
    }
}
