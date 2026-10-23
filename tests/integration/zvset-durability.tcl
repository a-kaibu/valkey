# ZVSET persistence and replication: RDB (DEBUG RELOAD, DUMP/RESTORE), AOF
# rewrite and replicas must reproduce every key exactly, in both encodings
# and across the edge cases of the score encoding.

# Fill the current database with zvset keys covering both encodings,
# 1/3/255 dimensions, infinities, -0, extreme doubles, binary and empty
# members, and a key larger than one AOF rewrite command.
proc zv_durability_dataset {} {
    r config set zset-max-listpack-entries 16
    r config set zset-max-listpack-value 32
    r flushall
    r zvadd zvd:compact 1#2#3 a -1#0#5 b inf#-inf#0 c
    r zvadd zvd:edge -0#1.7976931348623157e308#5e-324 zero -1e-300#-1.7976931348623157e308#0.1 tiny
    r zvadd zvd:edge 1#1#1 "" 2#2#2 "bin\x00ary\xff" 3#3#3 [string repeat x 40]
    set dims255 [join [lrepeat 255 7] #]
    r zvadd zvd:dims255 $dims255 m1 [join [lrepeat 255 -3.5] #] m2
    for {set i 0} {$i < 1000} {incr i} {
        lappend args "[expr {$i % 17}]#[expr {-$i}]" m$i
    }
    r zvadd zvd:big {*}$args
    r zvadd zvd:one 42 only
    assert_encoding compact zvd:compact
    assert_encoding btree zvd:edge
    assert_encoding btree zvd:big
}

proc zv_durability_state {} {
    set state {}
    foreach key [lsort [r keys zvd:*]] {
        lappend state $key [r object encoding $key] [r debug digest-value $key] [r zvrange $key 0 -1 withscores]
    }
    return $state
}

start_server {tags {"zvset external:skip needs:debug"} overrides {save {} sanitize-dump-payload yes}} {
    test "ZVSET survives DEBUG RELOAD with every encoding and edge value" {
        zv_durability_dataset
        set before [zv_durability_state]
        r debug reload
        assert_equal $before [zv_durability_state]
        assert_equal {0#1.7976931348623157e+308#5e-324} [r zvscore zvd:edge zero]
    }

    test "ZVSET survives AOF rewrite and reload" {
        zv_durability_dataset
        set before [zv_durability_state]
        r config set appendonly yes
        waitForBgrewriteaof r
        r debug loadaof
        assert_equal $before [zv_durability_state]
        r config set appendonly no
    }

    test "ZVSET DUMP/RESTORE round-trips every key" {
        zv_durability_dataset
        foreach key [r keys zvd:*] {
            set dump [r dump $key]
            r restore $key:copy 0 $dump
            assert_equal [r debug digest-value $key] [r debug digest-value $key:copy]
            assert_equal [r object encoding $key] [r object encoding $key:copy]
            r del $key:copy
        }
    }

    test "ZVSET RESTORE rejects corrupt payloads" {
        r debug set-skip-checksum-validation 1
        r del k
        r zvadd k 1#2 a 3#4 b
        set dump [r dump k]
        # Layout: type, len(2), dims(2), then member/score pairs. Each bad
        # payload below changes one of those fields.
        # Zero dimensions.
        assert_error "*Bad data format*" {r restore bad 0 [string replace $dump 2 2 [binary format c 0]] REPLACE}
        # Truncated item list (length says 3, two items follow).
        assert_error "*Bad data format*" {r restore bad 0 [string replace $dump 1 1 [binary format c 3]] REPLACE}
        # Duplicate member.
        set dup [string map {b a} $dump]
        assert_error "*Bad data format*" {r restore bad 0 $dup REPLACE}
        # NaN component.
        set nan [binary format w 0x7ff8000000000000]
        set at [string first [binary format q 4.0] $dump]
        assert {$at > 0}
        assert_error "*Bad data format*" {r restore bad 0 [string replace $dump $at [expr {$at + 7}] $nan] REPLACE}
        assert_equal 0 [r exists bad]
        r debug set-skip-checksum-validation 0
    }
}

start_server {tags {"zvset repl external:skip needs:debug"} overrides {save {}}} {
    start_server {overrides {save {}}} {
        set master [srv -1 client]
        set master_host [srv -1 host]
        set master_port [srv -1 port]
        set replica [srv 0 client]

        test "ZVSET full sync reproduces every key on the replica" {
            r -1 config set zset-max-listpack-entries 16
            r -1 zvadd zvd:compact 1#2#3 a -1#0#5 b inf#-inf#0 c
            for {set i 0} {$i < 1000} {incr i} {
                r -1 zvadd zvd:big "[expr {$i % 17}]#[expr {-$i}]#0" m$i
            }
            $replica replicaof $master_host $master_port
            wait_for_sync $replica
            wait_for_ofs_sync $master $replica
            assert_equal [$master debug digest] [$replica debug digest]
        }

        test "ZVSET commands propagate deterministically to the replica" {
            set rd [valkey_deferring_client -1]
            # A blocked pop served later must reach the replica as a plain pop.
            $rd bzvpopmin zvd:blocked 0
            wait_for_blocked_clients_count 1 100 10 -1
            r -1 zvadd zvd:blocked 5#5#5 x 6#6#6 y
            assert_equal {zvd:blocked x 5#5#5} [$rd read]
            $rd close
            r -1 zvincrby zvd:compact 1#1#1 a
            r -1 zvadd zvd:compact GT 0#0#0 b
            r -1 zvunionstore zvd:u 2 zvd:compact zvd:big aggregate max
            r -1 zvinterstore zvd:i 2 zvd:compact zvd:big
            r -1 zvdiffstore zvd:d 2 zvd:big zvd:compact
            r -1 zvrangestore zvd:r zvd:big 10 99
            r -1 zvremrangebyscore zvd:big 3#-inf#-inf "(5#0#0"
            r -1 zvremrangebyrank zvd:big 0 9
            r -1 zvpopmax zvd:big 3
            r -1 zvmpop 1 zvd:big min count 2
            r -1 zvrem zvd:big m500 m501
            wait_for_ofs_sync $master $replica
            assert_equal [$master debug digest] [$replica debug digest]
            assert_equal [r -1 zvrange zvd:big 0 -1 withscores] [$replica zvrange zvd:big 0 -1 withscores]
        }
    }
}
