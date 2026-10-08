# ZVSET (vector sorted set) PoC tests.
#
# ZV stores multiple double scores per member and orders by vector
# lexicographic comparison, then member bytes. Scores are given in
# Tair-style '#' separated protocol representation (e.g. "100#15#3").

start_server {tags {"zvset"}} {
    proc zv_create {key args} {
        r del $key
        if {[llength $args] > 0} {
            r zvadd $key {*}$args
        }
    }

    test "ZVADD basic add one, multiple, duplicate update, cardinality" {
        zv_create x 1 a 2 b 3 c
        assert_equal 0 [r zvadd x 1 a 2 b 3 c]
        # duplicate member with same score: no change
        assert_equal 0 [r zvadd x 1 a]
        # duplicate member with new score: update, not added
        assert_equal 0 [r zvadd x 9 a]
        assert_equal {9} [r zvscore x a]
        assert_equal 3 [r zvcard x]
        # multiple pairs in one call
        assert_equal 2 [r zvadd y 1#1 m1 2#2 m2]
        assert_equal 2 [r zvcard y]
    }

    test "ZVADD/ZVRANGE ordering from spec example" {
        zv_create x 1#10 c 1#2 b 1#2 a 0#999 d 2#0 e
        assert_equal {d a b c e} [r zvrange x 0 -1]
    }

    test "ZV ordering: numeric not lexicographic" {
        zv_create x 10 a 2 b 9 c
        assert_equal {b c a} [r zvrange x 0 -1]
    }

    test "ZV ordering: negative, decimal, inf, zero" {
        zv_create x -5 a -10 b 0 c
        assert_equal {b a c} [r zvrange x 0 -1]
        zv_create x 1.5 a 1.25 b 1.75 c
        assert_equal {b a c} [r zvrange x 0 -1]
        zv_create x inf a 100 b -inf c
        assert_equal {c b a} [r zvrange x 0 -1]
        zv_create x 0 a
        assert_equal 1 [r zvcard x]
        # -0.0 and +0.0 are the same score: same member re-add is a no-op
        assert_equal 0 [r zvadd x -0 a]
        assert_equal {0} [r zvscore x a]
        assert_equal {a} [r zvrange x 0 -1]
    }

    test "ZV ordering: equal vector breaks tie by member" {
        zv_create x 1#1 c 1#1 a 1#1 b
        assert_equal {a b c} [r zvrange x 0 -1]
    }

    test "ZV dimension: first insert wins, mismatch rejected" {
        r del x
        assert_equal 1 [r zvadd x 1#2 a]
        assert_equal 1 [r zvadd x 3#4 b]
        assert_error "*dimension*" {r zvadd x 1#2#3 c}
        assert_equal {a b} [r zvrange x 0 -1]
        # mixed dimensions within a single ZVADD are rejected atomically
        assert_error "*dimension*" {r zvadd x 5#6 d 7#8#9 e}
        assert_equal {} [r zvscore x d]
        assert_equal 2 [r zvcard x]
    }

    test "ZV dimension: invalid counts rejected" {
        r del x
        assert_error "*invalid vector*" {r zvadd x {} a}
        assert_error "*invalid vector*" {r zvadd x "#" a}
        assert_error "*invalid vector*" {r zvadd x "1#" a}
        assert_error "*invalid vector*" {r zvadd x "#1" a}
        assert_equal 0 [r exists x]
    }

    test "ZVADD flags NX XX CH" {
        zv_create x 1 a
        assert_equal 0 [r zvadd x NX 2 a]
        assert_equal {1} [r zvscore x a]
        assert_equal 1 [r zvadd x NX 1 b]
        assert_equal 0 [r zvadd x XX 1 c]
        assert_equal {} [r zvscore x c]
        assert_equal 0 [r zvadd x XX 2 a]
        assert_equal {2} [r zvscore x a]
        # CH counts updates too
        assert_equal 1 [r zvadd x CH 3 a]
        assert_equal 1 [r zvadd x CH 4 d]
        assert_error "*not compatible*" {r zvadd x NX XX 1 e}
    }

    test "ZVRANK first/middle/last/missing/update" {
        zv_create x 1 a 2 b 3 c
        assert_equal 0 [r zvrank x a]
        assert_equal 1 [r zvrank x b]
        assert_equal 2 [r zvrank x c]
        assert_equal {} [r zvrank x nokey-member]
        assert_equal {} [r zvrank nokey a]
        r zvadd x 0 c
        assert_equal 0 [r zvrank x c]
        assert_equal 1 [r zvrank x a]
        assert_equal 2 [r zvrank x b]
    }

    test "ZVRANGE indexes, negative, out of range, empty, WITHSCORES" {
        zv_create x 1 a 2 b 3 c 4 d
        assert_equal {a b c d} [r zvrange x 0 -1]
        assert_equal {b c} [r zvrange x 1 2]
        assert_equal {c d} [r zvrange x -2 -1]
        assert_equal {a b c d} [r zvrange x 0 100]
        assert_equal {} [r zvrange x 10 20]
        assert_equal {} [r zvrange x 3 1]
        assert_equal {} [r zvrange nokey 0 -1]
        assert_equal {a 1 b 2 c 3 d 4} [r zvrange x 0 -1 withscores]
        assert_equal {b 2 c 3} [r zvrange x 1 2 withscores]
        assert_equal {d 4} [r zvrange x -1 -1 withscores]
    }

    test "ZVREM existing/missing/last item deletes key" {
        zv_create x 1 a 2 b
        assert_equal 1 [r zvrem x a]
        assert_equal 0 [r zvrem x a]
        assert_equal 1 [r zvcard x]
        assert_equal 1 [r zvrem x b]
        assert_equal 0 [r exists x]
        assert_equal 0 [r zvrem nokey a]
    }

    test "ZVSCORE present/missing" {
        zv_create x 100#15#3 alice
        assert_equal {100#15#3} [r zvscore x alice]
        assert_equal {} [r zvscore x bob]
        assert_equal {} [r zvscore nokey alice]
    }

    test "ZVCARD" {
        zv_create x 1 a 2 b
        assert_equal 2 [r zvcard x]
        assert_equal 0 [r zvcard nokey]
    }

    test "ZV score parsing rejects bad input" {
        r del x
        assert_error "*invalid vector*" {r zvadd x nan a}
        assert_error "*invalid vector*" {r zvadd x 1#nan a}
        assert_error "*invalid vector*" {r zvadd x abc a}
        assert_error "*invalid vector*" {r zvadd x 1##2 a}
        assert_error "*syntax*" {r zvadd x 1#2 a bad-extra-token}
        assert_equal 0 [r exists x]
    }

    test "ZV wrong type errors" {
        r del x
        r set x str
        assert_error "*WRONGTYPE*" {r zvadd x 1 a}
        assert_error "*WRONGTYPE*" {r zvscore x a}
        assert_error "*WRONGTYPE*" {r zvrank x a}
        assert_error "*WRONGTYPE*" {r zvrange x 0 -1}
        assert_error "*WRONGTYPE*" {r zvcard x}
        assert_error "*WRONGTYPE*" {r zvrem x a}
    }

    test "ZV TYPE, COPY, RDB reload round-trip" {
        zv_create src 1#10 c 1#2 b 1#2 a
        assert_equal {zvset} [r type src]
        assert_equal 1 [r copy src dst]
        assert_equal {a b c} [r zvrange dst 0 -1]
        assert_equal {1#2} [r zvscore dst a]
        r debug reload
        assert_equal {zvset} [r type src]
        assert_equal {a b c} [r zvrange src 0 -1]
        assert_equal {1#10} [r zvscore src c]
        assert_equal {a b c} [r zvrange dst 0 -1]
    }

    # Reference model comparison: vector lexicographic, then member.
    proc zv_compare {a b} {
        set am [lindex $a 0]
        set bm [lindex $b 0]
        foreach x [lrange $a 1 end] y [lrange $b 1 end] {
            if {$x < $y} {return -1}
            if {$x > $y} {return 1}
        }
        if {$am < $bm} {return -1}
        if {$am > $bm} {return 1}
        return 0
    }

    proc zv_expected {ref} {
        set items {}
        dict for {m v} $ref {
            lappend items [concat [list $m] $v]
        }
        return [lsort -command zv_compare $items]
    }

    proc zv_check {key ref} {
        set expected [zv_expected $ref]
        assert_equal [dict size $ref] [r zvcard $key]
        set got [r zvrange $key 0 -1 withscores]
        assert_equal [expr {[llength $expected] * 2}] [llength $got]
        set i 0
        foreach item $expected {
            set m [lindex $item 0]
            set v [lrange $item 1 end]
            assert_equal $m [lindex $got $i]
            set gotscores [split [lindex $got [expr {$i + 1}]] "#"]
            assert_equal [llength $v] [llength $gotscores]
            foreach a $v b $gotscores {
                # numeric comparison: Tcl and C shortest-round-trip
                # formatting may differ textually ("123.0" vs "123").
                assert {double($a) == double($b)}
            }
            assert_equal [expr {$i / 2}] [r zvrank $key $m]
            incr i 2
        }
    }

    test "ZV randomized correctness vs reference model" {
        expr {srand(424242)}
        set key "zvrand"
        r del $key
        set ref {}
        set dims 3
        set pool {}
        for {set i 0} {$i < 200} {incr i} {
            lappend pool [format "m%04d" $i]
        }
        proc randscore {} {
            set kind [expr {int(rand() * 20)}]
            if {$kind == 0} {return "inf"}
            if {$kind == 1} {return "-inf"}
            if {$kind == 2} {return "0"}
            return [expr {int(rand() * 20000 - 10000) / 100.0}]
        }
        # seed with fixed ordering case
        r zvadd $key 1#10#1 c 1#2#3 b 1#2#1 a 0#999#0 d 2#0#5 e
        set ref [dict merge $ref {c {1 10 1} b {1 2 3} a {1 2 1} d {0 999 0} e {2 0 5}}]
        zv_check $key $ref
        for {set step 0} {$step < 1500} {incr step} {
            set op [expr {int(rand() * 10)}]
            if {$op < 6} {
                set m [lindex $pool [expr {int(rand() * 200)}]]
                set v {}
                for {set k 0} {$k < $dims} {incr k} {
                    lappend v [randscore]
                }
                set scorestr [join $v "#"]
                r zvadd $key $scorestr $m
                # reference parses the same decimal strings
                set nv {}
                foreach s $v {
                    lappend nv [expr {double($s)}]
                }
                dict set ref $m $nv
            } elseif {$op < 8} {
                set m [lindex $pool [expr {int(rand() * 200)}]]
                r zvrem $key $m
                dict unset ref $m
            }
            if {$step % 150 == 0} {
                zv_check $key $ref
            }
        }
        zv_check $key $ref
    }
}
