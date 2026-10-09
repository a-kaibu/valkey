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

    test "ZVADD flags NX XX CH" {        zv_create x 1 a
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

    test "ZVADD GT LT vector comparison" {
        zv_create x 5#100 a 6#0 b
        # GT updates only when the new vector is lexicographically greater.
        # [5,200] > [5,100]: second component decides.
        assert_equal 0 [r zvadd x GT 5#200 a]
        assert_equal {5#200} [r zvscore x a]
        # [4,999] < [5,200]: first component decides, no update.
        assert_equal 0 [r zvadd x GT 4#999 a]
        assert_equal {5#200} [r zvscore x a]
        assert_equal 0 [r zvadd x LT 4#999 a]
        assert_equal {4#999} [r zvscore x a]
        assert_equal 0 [r zvadd x LT 4#1000 a]
        assert_equal {4#999} [r zvscore x a]
        # GT/LT on missing member behaves like plain add (no NX/XX).
        assert_equal 1 [r zvadd x GT 1#1 c]
        assert_equal 1 [r zvadd x LT 2#2 d]
        assert_error "*not compatible*" {r zvadd x GT LT 1#1 e}
        assert_error "*not compatible*" {r zvadd x NX GT 1#1 e}
        assert_error "*not compatible*" {r zvadd x NX LT 1#1 e}
    }

    test "ZVADD INCR and ZVINCRBY" {
        zv_create x 10#20 a
        assert_equal {12#15} [r zvadd x INCR 2#-5 a]
        assert_equal {12#15} [r zvscore x a]
        # missing member starts from zero vector
        assert_equal {3#4} [r zvadd x INCR 3#4 b]
        assert_equal {3#4} [r zvscore x b]
        assert_equal {5#5} [r zvincrby x 2#1 b]
        # XX on missing member: null, no creation
        assert_equal {} [r zvadd x XX INCR 1#1 c]
        assert_equal 0 [r exists c]
        # missing key without XX: created like ZINCRBY
        assert_equal {1#1} [r zvincrby newkey 1#1 m]
        assert_equal {1#1} [r zvscore newkey m]
        r del newkey
        # INCR accepts a single pair only
        assert_error "*single*" {r zvadd x INCR 1#1 a 2#2 b}
        # +inf + -inf = NaN: error, member unchanged
        r zvadd x inf#1 n
        assert_error "*NaN*" {r zvincrby x -inf#0 n}
        assert_equal {inf#1} [r zvscore x n]
        # signed zero canonicalization
        assert_equal {0#0} [r zvincrby x -0#-0 z]
    }

    test "ZVMSCORE" {
        zv_create x 1#2 a 3#4 b
        assert_equal {1#2 3#4 {}} [r zvmscore x a b nok]
        assert_equal {{} {}} [r zvmscore nokey a b]
    }

    test "ZVRANK WITHSCORE and ZVREVRANK" {
        zv_create x 1 a 2 b 3 c
        assert_equal {0 1} [r zvrank x a withscore]
        assert_equal {2 3} [r zvrank x c withscore]
        assert_equal 2 [r zvrevrank x a]
        assert_equal 1 [r zvrevrank x b]
        assert_equal 0 [r zvrevrank x c]
        assert_equal {0 3} [r zvrevrank x c withscore]
        assert_equal {} [r zvrevrank x nok]
        # reverse rank follows score updates
        r zvadd x 0 c
        assert_equal 2 [r zvrevrank x c]
        assert_equal 0 [r zvrevrank x b]
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

    test "ZVRANGE BYSCORE inclusive exclusive unbounded" {
        zv_create x 1#0 a 1#1 b 1#1 c 1#2 d 2#0 e
        assert_equal {b c d} [r zvrange x 1#1 1#2 byscore]
        assert_equal {d} [r zvrange x (1#1 1#2 byscore]
        assert_equal {b c} [r zvrange x 1#1 (1#2 byscore]
        assert_equal {} [r zvrange x (1#1 (1#1 byscore]
        assert_equal {a b c d e} [r zvrange x - + byscore]
        assert_equal {d e} [r zvrange x (1#1 + byscore]
        assert_equal {a b c} [r zvrange x - 1#1 byscore]
        # reversed bounds are empty
        assert_equal {} [r zvrange x 2#0 1#0 byscore]
        # bad bounds and dimension mismatch
        assert_error "*invalid vector*" {r zvrange x bad 1#1 byscore}
        assert_error "*invalid vector*" {r zvrange x 1 1#1 byscore}
        assert_equal {} [r zvrange nokey - + byscore]
        # empty member, NUL byte and 0xff members at the boundary
        set m0 ""
        set m1 "a\x00b"
        set m2 "a\xffc"
        r del y
        r zvadd y 1#2 $m0
        r zvadd y 1#2 $m1
        r zvadd y 1#2 $m2
        assert_equal [list $m0 $m1 $m2] [r zvrange y 1#2 1#2 byscore]
        assert_equal 3 [r zvcount y 1#2 1#2]
    }

    test "ZVRANGE REV and LIMIT" {
        zv_create x 1 a 2 b 3 c 4 d
        assert_equal {d c b a} [r zvrange x 0 -1 rev]
        assert_equal {c b} [r zvrange x 1 2 rev]
        assert_equal {d c b a} [r zvrange x 4 1 byscore rev]
        assert_equal {c b} [r zvrange x 4 1 byscore rev limit 1 2]
        assert_equal {b c} [r zvrange x - + byscore limit 1 2]
        assert_equal {b c d} [r zvrange x - + byscore limit 1 -1]
        assert_equal {} [r zvrange x - + byscore limit 0 0]
        assert_equal {} [r zvrange x - + byscore limit 10 5]
        assert_equal {a 1 b 2} [r zvrange x - + byscore limit 0 2 withscores]
        assert_error "*LIMIT*" {r zvrange x 0 -1 limit 0 2}
        assert_error "*non-negative*" {r zvrange x - + byscore limit -1 2}
    }

    test "ZVCOUNT" {
        zv_create x 1#0 a 1#1 b 1#1 c 1#2 d 2#0 e
        assert_equal 3 [r zvcount x 1#1 1#2]
        assert_equal 1 [r zvcount x (1#1 1#2]
        assert_equal 5 [r zvcount x - +]
        assert_equal 0 [r zvcount x 2#0 1#0]
        assert_equal 0 [r zvcount nokey - +]
        assert_error "*invalid vector*" {r zvcount x 1 2}
    }

    test "ZVREVRANGE and BYSCORE wrappers" {        zv_create x 1 a 2 b 3 c
        assert_equal {c b a} [r zvrevrange x 0 -1]
        assert_equal {c b} [r zvrevrange x 1 2]
        assert_equal {b 2 a 1} [r zvrevrange x 0 1 withscores]
        zv_create y 1#0 a 1#1 b 2#0 c
        assert_equal {a b} [r zvrangebyscore y 1#0 1#1]
        assert_equal {b a} [r zvrevrangebyscore y 1#1 1#0]
        assert_equal {c b} [r zvrevrangebyscore y 2#0 1#0 limit 0 2]
        assert_equal {b 1#1} [r zvrangebyscore y - + limit 1 1 withscores]
    }

    test "ZVRANGE BYLEX uniform keys" {
        zv_create u 1#1 a 1#1 b 1#1 c 1#1 d
        assert_equal {a b c d} [r zvrange u - + bylex]
        assert_equal {b c d} [r zvrange u {[b} {[d} bylex]
        assert_equal {c d} [r zvrange u {(b} + bylex]
        assert_equal {d c b} [r zvrange u {[d} {[b} bylex rev]
        assert_equal {c} [r zvrange u - + bylex limit 2 1]
        assert_equal {b 1#1 c 1#1} [r zvrange u - + bylex limit 1 2 withscores]
        assert_equal {a b} [r zvrangebylex u {[a} {[b}]
        assert_equal {d c b} [r zvrevrangebylex u {[d} {[b}]
        assert_equal 2 [r zvlexcount u {[b} {(d}]
        assert_equal 4 [r zvlexcount u - +]
    }

    test "ZVRANGE BYLEX rejects mixed vectors" {
        zv_create v 1#1 x 2#2 y
        assert_error "*same vector*" {r zvrange v {[a} {[z} bylex}
        assert_error "*same vector*" {r zvrangebylex v - +}
        assert_error "*same vector*" {r zvrevrangebylex v + -}
        assert_error "*same vector*" {r zvlexcount v - +}
        assert_error "*same vector*" {r zvremrangebylex v {[a} {[z}}
        assert_equal {} [r zvrange nokey {[a} {[z} bylex]
    }

    test "ZVRANGESTORE" {
        zv_create src 1 a 2 b 3 c 4 d
        assert_equal 2 [r zvrangestore rdst src 1 2]
        assert_equal {b c} [r zvrange rdst 0 -1]
        assert_equal {zvset} [r type rdst]
        # byscore + rev + limit (stored set reads back in vector order)
        assert_equal 2 [r zvrangestore rdst2 src 4 1 byscore rev limit 0 2]
        assert_equal {c d} [r zvrange rdst2 0 -1]
        # bylex on uniform source
        zv_create srclex 1#1 a 1#1 b 1#1 c
        assert_equal 2 [r zvrangestore rdst3 srclex {[b} {[c} bylex]
        assert_equal {b c} [r zvrange rdst3 0 -1]
        # src == dst works
        assert_equal 2 [r zvrangestore src src 1 2]
        assert_equal {b c} [r zvrange src 0 -1]
        # empty result deletes destination
        r zvadd keep 1#1 z
        assert_equal 0 [r zvrangestore keep src 5 9]
        assert_equal 0 [r exists keep]
        # missing source deletes destination, returns 0
        r zvadd keep2 1#1 z
        assert_equal 0 [r zvrangestore keep2 nosuchkey 0 -1]
        assert_equal 0 [r exists keep2]
        # dimension change replaces wholesale
        r zvadd other 1#1#1 q
        assert_equal 2 [r zvrangestore other src 0 1]
        assert_equal {b c} [r zvrange other 0 -1]
        assert_equal {2} [r zvscore other b]
    }

    test "ZVREMRANGEBYRANK" {
        zv_create x 1 a 2 b 3 c 4 d 5 e
        assert_equal 2 [r zvremrangebyrank x 0 1]
        assert_equal {c d e} [r zvrange x 0 -1]
        assert_equal 1 [r zvremrangebyrank x -1 -1]
        assert_equal {c d} [r zvrange x 0 -1]
        assert_equal 0 [r zvremrangebyrank x 5 9]
        assert_equal 0 [r zvremrangebyrank nokey 0 -1]
        # deleting everything removes the key
        assert_equal 2 [r zvremrangebyrank x 0 -1]
        assert_equal 0 [r exists x]
    }

    test "ZVREMRANGEBYSCORE" {
        zv_create x 1#0 a 1#1 b 1#1 c 1#2 d 2#0 e
        assert_equal 2 [r zvremrangebyscore x 1#1 (1#2]
        assert_equal {a d e} [r zvrange x 0 -1]
        assert_equal 3 [r zvremrangebyscore x - +]
        assert_equal 0 [r exists x]
        zv_create x 1 a 2 b
        assert_equal 0 [r zvremrangebyscore x 2 1]
        assert_error "*invalid vector*" {r zvremrangebyscore x bad -}
    }

    test "ZVREMRANGEBYLEX" {
        zv_create x 1#1 a 1#1 b 1#1 c 1#1 d
        assert_equal 2 [r zvremrangebylex x {[a} {[b}]
        assert_equal {c d} [r zvrange x 0 -1]
        assert_equal 2 [r zvremrangebylex x - +]
        assert_equal 0 [r exists x]
    }

    test "ZVPOPMIN ZVPOPMAX" {
        zv_create p 1#1 a 2#2 b 3#3 c
        assert_equal {a 1#1} [r zvpopmin p]
        assert_equal {c 3#3} [r zvpopmax p]
        assert_equal {b 2#2} [r zvpopmin p]
        assert_equal 0 [r exists p]
        assert_equal {} [r zvpopmin p]
        assert_equal {} [r zvpopmin nokey]
        # count exceeds size, count 0
        zv_create p 1 a 2 b
        assert_equal {a 1 b 2} [r zvpopmin p 10]
        assert_equal 0 [r exists p]
        zv_create p 1 a 2 b
        assert_equal {} [r zvpopmin p 0]
        assert_equal 2 [r zvcard p]
        # tie-break by member
        zv_create p 1#1 c 1#1 a 1#1 b
        assert_equal {a 1#1} [r zvpopmin p]
        assert_equal {c 1#1} [r zvpopmax p]
    }

    test "ZVMPOP multi-key order" {
        r del k1 k2
        r zvadd k2 1#1 x 2#2 y
        assert_equal {k2 {{x 1#1}}} [r zvmpop 2 k1 k2 min count 1]
        assert_equal {k2 {{y 2#2}}} [r zvmpop 2 k1 k2 max]
        assert_equal 0 [r exists k2]
        assert_equal {} [r zvmpop 1 nokey min]
        assert_error "*syntax*" {r zvmpop 1 k1 badwhere}
    }

    test "ZVRANDMEMBER" {
        zv_create q 1#1 x 2#2 y 3#3 z
        # single returns a member
        set one [r zvrandmember q]
        assert {[lsearch -exact {x y z} $one] >= 0}
        # positive count: distinct
        set got [r zvrandmember q 2]
        assert_equal 2 [llength $got]
        assert_equal 2 [llength [lsort -unique $got]]
        # full count returns all shuffled
        assert_equal {x y z} [lsort [r zvrandmember q 10]]
        # negative count allows duplicates
        set got [r zvrandmember q -10]
        assert_equal 10 [llength $got]
        # withscores flat pairs
        set got [r zvrandmember q -2 withscores]
        assert_equal 4 [llength $got]
        # distribution sanity: every member appears over many draws
        set seen {}
        for {set i 0} {$i < 60} {incr i} {
            dict incr seen [r zvrandmember q]
        }
        assert_equal {x y z} [lsort [dict keys $seen]]
        assert_equal {} [r zvrandmember nokey]
        assert_equal {} [r zvrandmember nokey 5]
    }

    test "ZVSCAN cursor match count" {        zv_create q 1#1 ax 2#2 bx 3#3 cy
        set all {}
        set cursor 0
        while 1 {
            lassign [r zvscan q $cursor] cursor items
            foreach {m s} $items {
                lappend all $m
            }
            if {$cursor == 0} break
        }
        assert_equal {ax bx cy} [lsort $all]
        # match filters members
        lassign [r zvscan q 0 match {*x}] cursor items
        assert_equal 0 $cursor
        set got {}
        foreach {m s} $items { lappend got $m }
        assert_equal {ax bx} [lsort $got]
        # scores come along
        lassign [r zvscan q 0 match cy] cursor items
        assert_equal {cy 3#3} $items
        assert_equal {0 {}} [r zvscan nokey 0]
        assert_error "*invalid cursor*" {r zvscan q bad}
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

    test "ZV binary member round-trip" {
        r del x
        set m1 "a b"
        set m2 [binary format "H*" "6100ff63"]
        assert_equal 1 [r zvadd x 1#2 $m1]
        assert_equal 1 [r zvadd x 3#4 $m2]
        assert_equal [list $m1 $m2] [r zvrange x 0 -1]
        assert_equal {3#4} [r zvscore x $m2]
        assert_equal 1 [r zvrank x $m2]
        assert_equal 0 [r zvrank x $m1]
        assert_equal 1 [r zvrem x $m2]
        assert_equal 1 [r zvcard x]
    }

    test "ZV 255 dimensions accepted, 256 rejected" {
        r del x
        set s255 [join [lrepeat 255 1] "#"]
        set s256 [join [lrepeat 256 1] "#"]
        assert_equal 1 [r zvadd x $s255 a]
        assert_equal 1 [r zvcard x]
        assert_equal $s255 [r zvscore x a]
        catch [list r zvadd x 1#2 c] err
        assert_match "*dimension*" $err
        catch [list r zvadd ydim256 $s256 a] err
        assert_match "*invalid vector*" $err
        assert_equal 0 [r exists ydim256]
    }

    test "ZV long common prefix orders by last dimension" {        zv_create x 5#5#5#5#5#5#5#30 m3 5#5#5#5#5#5#5#10 m1 5#5#5#5#5#5#5#20 m2
        assert_equal {m1 m2 m3} [r zvrange x 0 -1]
        assert_equal 0 [r zvrank x m1]
        assert_equal 2 [r zvrank x m3]
        # identical full vectors tie-break by member
        r zvadd x 5#5#5#5#5#5#5#10 m0
        assert_equal {m0 m1 m2 m3} [r zvrange x 0 -1]
    }

    test "ZV randomized correctness vs reference model" {        expr {srand(424242)}
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

start_server {tags {"zvset needs:debug"} overrides {appendonly yes aof-use-rdb-preamble no}} {
    test {AOF rewrite and reload preserve ZVSET values} {
        r zvadd ranking 100#15#3 alice 100#16#1 bob 101#1#9 carol
        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof
        assert_equal {zvset} [r type ranking]
        assert_equal {alice bob carol} [r zvrange ranking 0 -1]
        assert_equal {100#16#1} [r zvscore ranking bob]
        assert_equal 2 [r zvrank ranking carol]
        assert_equal 3 [r zvcard ranking]
    }
}

start_server {tags {"zvset-setops"}} {    proc zv_setup_setops {} {
        r del a b u v w
        r zvadd a 10#20 alice 20#0 bob 1#5 carol
        r zvadd b 3#7 alice 30#0 bob 4#9 dave
    }

    test "ZVUNION SUM default" {
        zv_setup_setops
        assert_equal {carol dave alice bob} [r zvunion 2 a b]
        assert_equal {carol 1#5 dave 4#9 alice 13#27 bob 50#0} [r zvunion 2 a b withscores]
    }

    test "ZVUNION MIN MAX are vector-wide" {
        zv_setup_setops
        # MIN picks whole vectors: alice=[3,7] (not [3,20]).
        assert_equal {carol alice dave bob} [r zvunion 2 a b aggregate min]
        assert_equal {carol 1#5 alice 3#7 dave 4#9 bob 20#0} [r zvunion 2 a b aggregate min withscores]
        assert_equal {carol dave alice bob} [r zvunion 2 a b aggregate max]
        assert_equal {carol 1#5 dave 4#9 alice 10#20 bob 30#0} [r zvunion 2 a b aggregate max withscores]
    }

    test "ZVUNION WEIGHTS" {
        zv_setup_setops
        assert_equal {carol dave alice bob} [r zvunion 2 a b weights 2 3]
        assert_equal {carol 2#10 dave 12#27 alice 29#61 bob 130#0} [r zvunion 2 a b weights 2 3 withscores]
        # negative weight
        assert_equal {bob -20#0 alice -10#-20 carol -1#-5} [r zvunion 1 a weights -1 withscores]
    }

    test "ZVINTER SUM and missing keys" {
        zv_setup_setops
        assert_equal {alice bob} [r zvinter 2 a b]
        assert_equal {alice 13#27 bob 50#0} [r zvinter 2 a b withscores]
        # missing keys are empty sets: intersection is empty
        assert_equal {} [r zvinter 3 a b nokey]
        assert_equal {} [r zvinter 2 a nokey]
        # duplicate input keys aggregate per input position (SUM doubles)
        assert_equal {carol 2#10 alice 20#40 bob 40#0} [r zvinter 2 a a withscores]
    }

    test "ZVDIFF keeps first-key vectors" {
        zv_setup_setops
        assert_equal {carol} [r zvdiff 2 a b]
        assert_equal {carol 1#5} [r zvdiff 2 a b withscores]
        assert_equal {carol alice bob} [r zvdiff 2 a nokey]
        assert_equal {} [r zvdiff 2 nokey a]
    }

    test "ZVINTERCARD with LIMIT" {
        zv_setup_setops
        assert_equal 2 [r zvintercard 2 a b]
        assert_equal 1 [r zvintercard 2 a b limit 1]
        assert_equal 0 [r zvintercard 2 a nokey]
        assert_error "*negative*" {r zvintercard 2 a b limit -1}
    }

    test "ZVUNIONSTORE INTERSTORE DIFFSTORE" {
        zv_setup_setops
        assert_equal 4 [r zvunionstore u 2 a b]
        assert_equal {carol dave alice bob} [r zvrange u 0 -1]
        assert_equal 2 [r zvinterstore v 2 a b]
        assert_equal {alice bob} [r zvrange v 0 -1]
        assert_equal 1 [r zvdiffstore w 2 a b]
        assert_equal {carol} [r zvrange w 0 -1]
        # empty result deletes destination
        r zvadd keep 1#1 z
        assert_equal 0 [r zvinterstore keep 2 a nokey2]
        assert_equal 0 [r exists keep]
        # dst == src works (vectors doubled by self-union SUM)
        assert_equal 3 [r zvunionstore a 1 a]
        assert_equal {carol alice bob} [r zvrange a 0 -1]
        # dimension mismatch
        r zvadd dd 1#2#3 q
        assert_error "*dimension*" {r zvunion 2 a dd}
        assert_error "*dimension*" {r zvunionstore u2 2 a dd}
        # wrong type
        r set s str
        assert_error "*WRONGTYPE*" {r zvunion 2 a s}
    }
}

start_server {tags {"zvset-blocking"}} {
    test "BZVPOPMIN wake-up on ZVADD" {
        r del bk
        set rd [valkey_deferring_client]
        $rd bzvpopmin bk 5
        wait_for_blocked_clients_count 1
        r zvadd bk 1#1 a 2#2 b
        assert_equal {bk a 1#1} [$rd read]
        assert_equal {b} [r zvrange bk 0 -1]
        $rd close
    }

    test "BZVPOPMAX timeout returns nil" {
        r del bk
        set rd [valkey_deferring_client]
        $rd bzvpopmax bk 1
        assert_equal {} [$rd read]
        $rd close
    }

    test "BZVPOPMIN multi-key order" {
        r del k1 k2
        r zvadd k2 1#1 x
        set rd [valkey_deferring_client]
        $rd bzvpopmin k1 k2 5
        assert_equal {k2 x 1#1} [$rd read]
        $rd close
    }

    test "BZVMPOP wake-up and count" {
        r del bk
        set rd [valkey_deferring_client]
        $rd bzvmpop 0 1 bk min count 2
        wait_for_blocked_clients_count 1
        r zvadd bk 1#1 a 2#2 b 3#3 c
        assert_equal {bk {{a 1#1} {b 2#2}}} [$rd read]
        assert_equal {c} [r zvrange bk 0 -1]
        $rd close
    }

    test "BZVPOPMIN on wrong type errors" {
        r set s str
        set rd [valkey_deferring_client]
        $rd bzvpopmin s 1
        assert_error "*WRONGTYPE*" {$rd read}
        $rd close
    }

    test "BZVPOPMIN woken by ZSET creation gets WRONGTYPE safely" {
        r del bk
        set rd [valkey_deferring_client]
        $rd bzvpopmin bk 5
        wait_for_blocked_clients_count 1
        r zadd bk 1 m
        assert_error "*WRONGTYPE*" {$rd read}
        $rd close
    }
}
