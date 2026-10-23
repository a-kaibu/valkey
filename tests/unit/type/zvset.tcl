# ZVSET (vector sorted set) PoC tests.
#
# ZV stores multiple double scores per member and orders by vector
# lexicographic comparison, then member bytes. Scores are given in
# Tair-style '#' separated protocol representation (e.g. "100#15#3").

# Every block runs once per encoding: compact (small keys) and btree
# (zset-max-listpack-entries 0 forces btree from the first member).
foreach {zv_encoding zv_max_entries} {compact 128 btree 0} {
    start_server {tags {"zvset"}} {
        r config set zset-max-listpack-entries $zv_max_entries
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

        test "ZV score components: integer fast path and float fallback agree" {
            r del x
            # Integers up to 15 digits take the fast path; longer ones,
            # signs other than '-', exponents and fractions use string2d.
            r zvadd x -0#007#+3#1e2#-12#999999999999999#1234567890123456#2.5 a
            assert_equal {0#7#3#100#-12#999999999999999#1234567890123456#2.5} [r zvscore x a]
            foreach bad {- -- --1 1-2 1a 0x10 " 1" nan} {
                assert_error "*invalid vector*" {r zvadd x "$bad#0#0#0#0#0#0#0" b}
            }
            assert_equal 1 [r zvcard x]
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
            zv_create x 1 a 2 b 3 c 4 d 5 e
            assert_equal {e d c b a} [r zvrange x 0 -1 rev]
            # REV ranks count from the head of the reversed set
            assert_equal {e d} [r zvrange x 0 1 rev]
            assert_equal {d c} [r zvrange x 1 2 rev]
            assert_equal {a} [r zvrange x 4 4 rev]
            assert_equal {d c b a} [r zvrange x 4 1 byscore rev]
            assert_equal {c b} [r zvrange x 4 1 byscore rev limit 1 2]
            assert_equal {b c} [r zvrange x - + byscore limit 1 2]
            assert_equal {b c d e} [r zvrange x - + byscore limit 1 -1]
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

        test "ZVREVRANGE and BYSCORE wrappers" {
            zv_create x 1 a 2 b 3 c
            assert_equal {c b a} [r zvrevrange x 0 -1]
            assert_equal {b a} [r zvrevrange x 1 2]
            assert_equal {c 3 b 2} [r zvrevrange x 0 1 withscores]
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

        test "ZVRANGE BYLEX prefix-sharing members" {
            zv_create u 1#1 a 1#1 ab 1#1 b
            # exclusive lower must not skip "ab"
            assert_equal {ab b} [r zvrange u {(a} + bylex]
            assert_equal {ab b} [r zvrangebylex u {(a} +]
            # inclusive upper must not include "ab"
            assert_equal {a} [r zvrange u - {[a} bylex]
            assert_equal 1 [r zvlexcount u - {[a}]
            assert_equal {a ab} [r zvrange u - {(b} bylex]
            assert_equal {ab b} [r zvrange u {[ab} + bylex]
            # NUL byte member sorts between "a" and "ab"
            set mnul "a\x00"
            r zvadd u 1#1 $mnul
            assert_equal [list a $mnul ab b] [r zvrange u - + bylex]
            assert_equal [list $mnul ab b] [r zvrange u {(a} + bylex]
            assert_equal [list a] [r zvrange u - {[a} bylex]
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
            zv_create src{zvr} 1 a 2 b 3 c 4 d
            assert_equal 2 [r zvrangestore rdst{zvr} src{zvr} 1 2]
            assert_equal {b c} [r zvrange rdst{zvr} 0 -1]
            assert_equal {zvset} [r type rdst{zvr}]
            # byscore + rev + limit (stored set reads back in vector order)
            assert_equal 2 [r zvrangestore rdst2{zvr} src{zvr} 4 1 byscore rev limit 0 2]
            assert_equal {c d} [r zvrange rdst2{zvr} 0 -1]
            # bylex on uniform source
            zv_create srclex{zvr} 1#1 a 1#1 b 1#1 c
            assert_equal 2 [r zvrangestore rdst3{zvr} srclex{zvr} {[b} {[c} bylex]
            assert_equal {b c} [r zvrange rdst3{zvr} 0 -1]
            # src == dst works
            assert_equal 2 [r zvrangestore src{zvr} src{zvr} 1 2]
            assert_equal {b c} [r zvrange src{zvr} 0 -1]
            # empty result deletes destination
            r zvadd keep{zvr} 1#1 z
            assert_equal 0 [r zvrangestore keep{zvr} src{zvr} 5 9]
            assert_equal 0 [r exists keep{zvr}]
            # missing source deletes destination, returns 0
            r zvadd keep2{zvr} 1#1 z
            assert_equal 0 [r zvrangestore keep2{zvr} nosuchkey{zvr} 0 -1]
            assert_equal 0 [r exists keep2{zvr}]
            # wrong-type destination is replaced, not an error
            r set strdst{zvr} str
            assert_equal 2 [r zvrangestore strdst{zvr} src{zvr} 0 1]
            assert_equal {zvset} [r type strdst{zvr}]
            assert_equal {b c} [r zvrange strdst{zvr} 0 -1]
            # dimension change replaces wholesale
            r zvadd other{zvr} 1#1#1 q
            assert_equal 2 [r zvrangestore other{zvr} src{zvr} 0 1]
            assert_equal {b c} [r zvrange other{zvr} 0 -1]
            assert_equal {2} [r zvscore other{zvr} b]
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

        test "ZVREMRANGE reversed infinities delete nothing" {
            zv_create x 1 a 2 b 3 c
            assert_equal 0 [r zvremrangebyscore x + -]
            assert_equal {a b c} [r zvrange x 0 -1]
            assert_equal 3 [r zvcard x]
            zv_create y 1#1 a 1#1 b 1#1 c
            assert_equal 0 [r zvremrangebylex y + -]
            assert_equal {a b c} [r zvrange y 0 -1]
            # reads also treat reversed infinities as empty
            assert_equal {} [r zvrange x + - byscore]
            assert_equal 0 [r zvcount x + -]
            assert_equal {} [r zvrange y + - bylex]
        }

        test "ZVRANGE adjacent-double boundaries" {
            r del x
            r zvadd x 1.0000000000000566 a 1.0000000000000568 b
            assert_equal {a} [r zvrange x - 1.0000000000000566 byscore]
            assert_equal {b} [r zvrange x (1.0000000000000566 +inf byscore]
            assert_equal 1 [r zvcount x - 1.0000000000000566]
            assert_equal {a} [r zvrange x -inf 1.0000000000000566 byscore]
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
            r del k1{zvm} k2{zvm}
            r zvadd k2{zvm} 1#1 x 2#2 y
            assert_equal {k2{zvm} {{x 1#1}}} [r zvmpop 2 k1{zvm} k2{zvm} min count 1]
            assert_equal {k2{zvm} {{y 2#2}}} [r zvmpop 2 k1{zvm} k2{zvm} max]
            assert_equal 0 [r exists k2{zvm}]
            assert_equal {} [r zvmpop 1 nokey min]
            assert_error "*syntax*" {r zvmpop 1 k1{zvm} badwhere}
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

        test "ZVRANDMEMBER sparse, dense and full samples across tree leaves" {
            r del random-many
            set pairs {}
            set scores {}
            for {set i 0} {$i < 1024} {incr i} {
                set member "m$i"
                set score "7#$i"
                lappend pairs $score $member
                dict set scores $member $score
            }
            foreach member [list "" "binary\x00\xff"] {
                lappend pairs 7#1024 $member
                dict set scores $member 7#1024
            }
            r zvadd random-many {*}$pairs
            set original [r zvrange random-many 0 -1 withscores]
            set n [dict size $scores]
            foreach count {0 1 10 255 256 257 341 342 343 512 1025 1026 2048} {
                set expected [expr {min($count, $n)}]
                set sample [r zvrandmember random-many $count]
                assert_equal $expected [llength $sample]
                assert_equal $expected [llength [lsort -unique $sample]]
                foreach member $sample {assert {[dict exists $scores $member]}}
                set sample [r zvrandmember random-many $count withscores]
                assert_equal [expr {$expected * 2}] [llength $sample]
                set members {}
                foreach {member score} $sample {
                    assert_equal [dict get $scores $member] $score
                    lappend members $member
                }
                assert_equal $expected [llength [lsort -unique $members]]
            }
            r hello 3
            set sample [r zvrandmember random-many 512 withscores]
            assert_equal 512 [llength $sample]
            foreach pair $sample {
                assert_equal 2 [llength $pair]
                lassign $pair member score
                assert_equal [dict get $scores $member] $score
            }
            r hello 2
            assert_equal $original [r zvrange random-many 0 -1 withscores]
        }

        test "ZVRANDMEMBER extreme counts are rejected safely" {
            zv_create q 1#1 x 2#2 y
            assert_error "*out of range*" {r zvrandmember q -9223372036854775808 withscores}
            assert_error "*out of range*" {r zvrandmember q 9223372036854775807 withscores}
            assert_error "*out of range*" {r zvrandmember q -9223372036854775808}
            # server still alive and data intact
            assert_equal 2 [r zvcard q]
            assert_equal {x y} [r zvrange q 0 -1]
        }

        test "ZV WITHSCORES nests pairs in RESP3" {
            zv_create q 1#1 x 2#2 y
            r hello 3
            assert_equal {{{x 1#1} {y 2#2}}} [list [r zvrange q 0 -1 withscores]]
            assert_equal {{{y 2#2} {x 1#1}}} [list [r zvrange q 0 -1 withscores rev]]
            lassign [r zvrandmember q 2 withscores] p1 p2
            assert_equal 2 [llength $p1]
            assert_equal 2 [llength $p2]
            assert_equal {{{x 1#1} {y 2#2}}} [list [lsort [r zvquery q filter 0 0 10 withscores]]]
            r hello 2
            # back to flat in RESP2
            assert_equal {x 1#1 y 2#2} [r zvrange q 0 -1 withscores]
        }

        test "ZVSCAN cursor match count" {
            zv_create q 1#1 ax 2#2 bx 3#3 cy
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

        test "ZV TYPE and COPY" {
            zv_create src{zv} 1#10 c 1#2 b 1#2 a
            assert_equal {zvset} [r type src{zv}]
            assert_equal 1 [r copy src{zv} dst{zv}]
            assert_equal {a b c} [r zvrange dst{zv} 0 -1]
            assert_equal {1#2} [r zvscore dst{zv} a]
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

        test "ZV long common prefix orders by last dimension" {
            zv_create x 5#5#5#5#5#5#5#30 m3 5#5#5#5#5#5#5#10 m1 5#5#5#5#5#5#5#20 m2
            assert_equal {m1 m2 m3} [r zvrange x 0 -1]
            assert_equal 0 [r zvrank x m1]
            assert_equal 2 [r zvrank x m3]
            # identical full vectors tie-break by member
            r zvadd x 5#5#5#5#5#5#5#10 m0
            assert_equal {m0 m1 m2 m3} [r zvrange x 0 -1]
        }

        test "ZV randomized correctness vs reference model" {
            set seed 424242
            puts "zv fuzz seed: $seed"
            expr {srand($seed)}
            set key "zvrand{zvf}"
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
            proc zv_in_range {v minv maxv} {
                # lexicographic [minv,maxv] containment over doubles
                set n [llength $v]
                for {set i 0} {$i < $n} {incr i} {
                    set x [lindex $v $i]
                    set lo [lindex $minv $i]
                    if {$x < $lo} {return 0}
                    if {$x > $lo} {break}
                }
                for {set i 0} {$i < $n} {incr i} {
                    set x [lindex $v $i]
                    set hi [lindex $maxv $i]
                    if {$x > $hi} {return 0}
                    if {$x < $hi} {break}
                }
                return 1
            }
            proc randvec {dims} {
                set v {}
                for {set k 0} {$k < $dims} {incr k} {
                    lappend v [randscore]
                }
                return $v
            }
            for {set step 0} {$step < 1500} {incr step} {
                set op [expr {int(rand() * 14)}]
                if {$op < 6} {
                    set m [lindex $pool [expr {int(rand() * 200)}]]
                    set v [randvec $dims]
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
                } elseif {$op == 8} {
                    # bulk delete by rank slice
                    set size [dict size $ref]
                    if {$size > 0} {
                        set start [expr {int(rand() * $size)}]
                        set stop [expr {$start + int(rand() * 10)}]
                        set victims [lrange [zv_expected $ref] $start $stop]
                        r zvremrangebyrank $key $start $stop
                        foreach victim $victims {
                            dict unset ref [lindex $victim 0]
                        }
                    }
                } elseif {$op == 9} {
                    # bulk delete by inclusive score range
                    set minv {}
                    set maxv {}
                    foreach s [randvec $dims] {lappend minv [expr {double($s)}]}
                    foreach s [randvec $dims] {lappend maxv [expr {double($s)}]}
                    set smin [join $minv "#"]
                    set smax [join $maxv "#"]
                    # order bounds lexicographically so ranges are non-trivial
                    if {[zv_compare [list _ {*}$minv] [list _ {*}$maxv]] > 0} {
                        set t $smin
                        set smin $smax
                        set smax $t
                        set t $minv
                        set minv $maxv
                        set maxv $t
                    }
                    r zvremrangebyscore $key $smin $smax
                    dict for {m v} $ref {
                        if {[zv_in_range $v $minv $maxv]} {
                            dict unset ref $m
                        }
                    }
                } elseif {$op == 10} {
                    # pop the minimum
                    if {[dict size $ref] > 0} {
                        set first [lindex [zv_expected $ref] 0]
                        r zvpopmin $key
                        dict unset ref [lindex $first 0]
                    }
                } elseif {$op == 11} {
                    # copy round-trip
                    r copy $key ${key}:copy
                    zv_check ${key}:copy $ref
                    r del ${key}:copy
                }
                if {$step % 150 == 0} {
                    zv_check $key $ref
                    # spot-check COUNT against the reference
                    set minv {}
                    set maxv {}
                    foreach s [randvec $dims] {lappend minv [expr {double($s)}]}
                    foreach s [randvec $dims] {lappend maxv [expr {double($s)}]}
                    if {[zv_compare [list _ {*}$minv] [list _ {*}$maxv]] > 0} {
                        set t $minv
                        set minv $maxv
                        set maxv $t
                    }
                    set want 0
                    dict for {m v} $ref {
                        if {[zv_in_range $v $minv $maxv]} {incr want}
                    }
                    assert_equal $want [r zvcount $key [join $minv "#"] [join $maxv "#"]]
                }
            }
            zv_check $key $ref
        }
    }

    start_server {tags {"zvset needs:debug"} overrides {appendonly yes aof-use-rdb-preamble no}} {
        r config set zset-max-listpack-entries $zv_max_entries
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

        test {RDB reload preserves ZVSET values} {
            r zvadd rdbkey 1#10 c 1#2 b 1#2 a
            r debug reload
            assert_equal {zvset} [r type rdbkey]
            assert_equal {a b c} [r zvrange rdbkey 0 -1]
            assert_equal {1#10} [r zvscore rdbkey c]
        }
    }

    start_server {tags {"zvset-setops"}} {
        r config set zset-max-listpack-entries $zv_max_entries
        proc zv_setup_setops {} {
            r del a{zvs} b{zvs} u{zvs} v{zvs} w{zvs}
            r zvadd a{zvs} 10#20 alice 20#0 bob 1#5 carol
            r zvadd b{zvs} 3#7 alice 30#0 bob 4#9 dave
        }

        test "ZVUNION SUM default" {
            zv_setup_setops
            assert_equal {carol dave alice bob} [r zvunion 2 a{zvs} b{zvs}]
            assert_equal {carol 1#5 dave 4#9 alice 13#27 bob 50#0} [r zvunion 2 a{zvs} b{zvs} withscores]
        }

        test "ZVUNION MIN MAX are vector-wide" {
            zv_setup_setops
            # MIN picks whole vectors: alice=[3,7] (not [3,20]).
            assert_equal {carol alice dave bob} [r zvunion 2 a{zvs} b{zvs} aggregate min]
            assert_equal {carol 1#5 alice 3#7 dave 4#9 bob 20#0} [r zvunion 2 a{zvs} b{zvs} aggregate min withscores]
            assert_equal {carol dave alice bob} [r zvunion 2 a{zvs} b{zvs} aggregate max]
            assert_equal {carol 1#5 dave 4#9 alice 10#20 bob 30#0} [r zvunion 2 a{zvs} b{zvs} aggregate max withscores]
        }

        test "ZVUNION WEIGHTS" {
            zv_setup_setops
            assert_equal {carol dave alice bob} [r zvunion 2 a{zvs} b{zvs} weights 2 3]
            assert_equal {carol 2#10 dave 12#27 alice 29#61 bob 130#0} [r zvunion 2 a{zvs} b{zvs} weights 2 3 withscores]
            # negative weight
            assert_equal {bob -20#0 alice -10#-20 carol -1#-5} [r zvunion 1 a{zvs} weights -1 withscores]
        }

        test "ZVINTER SUM and missing keys" {
            zv_setup_setops
            assert_equal {alice bob} [r zvinter 2 a{zvs} b{zvs}]
            assert_equal {alice 13#27 bob 50#0} [r zvinter 2 a{zvs} b{zvs} withscores]
            # missing keys are empty sets: intersection is empty
            assert_equal {} [r zvinter 3 a{zvs} b{zvs} nokey]
            assert_equal {} [r zvinter 2 a{zvs} nokey]
            # duplicate input keys aggregate per input position (SUM doubles)
            assert_equal {carol 2#10 alice 20#40 bob 40#0} [r zvinter 2 a{zvs} a{zvs} withscores]
        }

        test "ZVDIFF keeps first-key vectors" {
            zv_setup_setops
            assert_equal {carol} [r zvdiff 2 a{zvs} b{zvs}]
            assert_equal {carol 1#5} [r zvdiff 2 a{zvs} b{zvs} withscores]
            assert_equal {carol alice bob} [r zvdiff 2 a{zvs} nokey]
            assert_equal {} [r zvdiff 2 nokey a{zvs}]
        }

        test "ZVINTERCARD with LIMIT" {
            zv_setup_setops
            assert_equal 2 [r zvintercard 2 a{zvs} b{zvs}]
            assert_equal 1 [r zvintercard 2 a{zvs} b{zvs} limit 1]
            assert_equal 0 [r zvintercard 2 a{zvs} nokey]
            assert_error "*negative*" {r zvintercard 2 a{zvs} b{zvs} limit -1}
            assert_equal 3 [r zvintercard 1 a{zvs}]
            assert_equal 3 [r zvintercard 1 a{zvs} limit 0]
            assert_equal 1 [r zvintercard 1 a{zvs} limit 1]
            assert_equal 3 [r zvintercard 1 a{zvs} limit 10]
            assert_equal 0 [r zvintercard 1 nokey limit 1]
            assert_error "*negative*" {r zvintercard 1 a{zvs} limit -1}
        }

        test "ZVUNIONSTORE INTERSTORE DIFFSTORE" {
            zv_setup_setops
            assert_equal 4 [r zvunionstore u{zvs} 2 a{zvs} b{zvs}]
            assert_equal {carol dave alice bob} [r zvrange u{zvs} 0 -1]
            assert_equal 2 [r zvinterstore v{zvs} 2 a{zvs} b{zvs}]
            assert_equal {alice bob} [r zvrange v{zvs} 0 -1]
            assert_equal 1 [r zvdiffstore w{zvs} 2 a{zvs} b{zvs}]
            assert_equal {carol} [r zvrange w{zvs} 0 -1]
            # empty result deletes destination
            r zvadd keep{zvs} 1#1 z
            assert_equal 0 [r zvinterstore keep{zvs} 2 a{zvs} nokey2{zvs}]
            assert_equal 0 [r exists keep{zvs}]
            # dst == src works (vectors doubled by self-union SUM)
            assert_equal 3 [r zvunionstore a{zvs} 1 a{zvs}]
            assert_equal {carol alice bob} [r zvrange a{zvs} 0 -1]
            # dimension mismatch
            r zvadd dd{zvs} 1#2#3 q
            assert_error "*dimension*" {r zvunion 2 a{zvs} dd{zvs}}
            assert_error "*dimension*" {r zvunionstore u2{zvs} 2 a{zvs} dd{zvs}}
            # wrong type
            r set s{zvs} str
            assert_error "*WRONGTYPE*" {r zvunion 2 a{zvs} s{zvs}}
        }

        test "ZVUNION inf weights and NaN normalization" {
            zv_setup_setops
            # +inf and -inf weights on the same member: NaN normalizes to 0
            # (0*inf weights apply first: bob=[20,0]*inf=[inf,0], then SUM
            # with [30,0] keeps [inf,0]; alice=[inf,inf]+[-inf,-inf]=[0,0]).
            assert_equal {dave 4#9 bob inf#0 alice inf#inf carol inf#inf} \
                [r zvunion 2 a{zvs} b{zvs} weights inf 1 withscores]
            # +inf and -inf weights on the same member: NaN normalizes to 0
            r zvunionstore tmp{zvs} 2 a{zvs} a{zvs} weights inf -inf
            assert_equal {0#0} [r zvscore tmp{zvs} alice]
            r del tmp{zvs}
        }

        test "ZV command flags match ZSET classification" {
            foreach {cmd wantreadonly wantslow wantwrite} {
                zvunion 1 1 0
                zvinter 1 1 0
                zvdiff 1 1 0
                zvintercard 1 1 0
                zvunionstore 0 1 1
                zvinterstore 0 1 1
                zvdiffstore 0 1 1
                zvmpop 0 1 1
                zvremrangebyrank 0 1 1
                zvremrangebyscore 0 1 1
                zvremrangebylex 0 1 1
            } {
                set info [lindex [r command info $cmd] 0]
                set flags [lindex $info 2]
                set acl [lindex $info 6]
                assert_equal $wantreadonly [expr {[lsearch -exact $flags readonly] >= 0}]
                assert_equal $wantslow [expr {[lsearch -exact $acl @slow] >= 0}]
                assert_equal $wantwrite [expr {[lsearch -exact $flags write] >= 0}]
            }
        }

        test "ZV read-only ACL user can run setops reads" {
            r acl setuser zvreader on nopass +@read ~* -@write -@connection
            assert_equal {OK} [r acl dryrun zvreader zvunion 2 a{zvs} b{zvs}]
            assert_equal {OK} [r acl dryrun zvreader zvintercard 2 a{zvs} b{zvs}]
            assert_match "*no permissions*" [r acl dryrun zvreader zvadd k 1#1 m]
            assert_match "*no permissions*" [r acl dryrun zvreader zvunionstore d 1 a{zvs}]
            r acl deluser zvreader
        }
    }

    start_server {tags {"zvset-blocking"}} {
        r config set zset-max-listpack-entries $zv_max_entries
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
            r del k1{zvm} k2{zvm}
            r zvadd k2{zvm} 1#1 x
            set rd [valkey_deferring_client]
            $rd bzvpopmin k1{zvm} k2{zvm} 5
            assert_equal {k2{zvm} x 1#1} [$rd read]
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

        test "BZVPOPMIN keeps blocking when a ZSET is created at the key" {
            r del bk
            set rd [valkey_deferring_client]
            $rd bzvpopmin bk 0
            wait_for_blocked_clients_count 1
            r zadd bk 1 m
            # A ZSET must not wake a ZVSET waiter (and vice versa).
            wait_for_blocked_clients_count 1
            r del bk
            r zvadd bk 1#2 m
            assert_equal {bk m 1#2} [$rd read]
            $rd close
        }

        test "BZPOPMIN keeps blocking when a ZVSET is created at the key" {
            r del bk
            set rd [valkey_deferring_client]
            $rd bzpopmin bk 0
            wait_for_blocked_clients_count 1
            r zvadd bk 1#2 m
            wait_for_blocked_clients_count 1
            r del bk
            r zadd bk 3 m
            assert_equal {bk m 3} [$rd read]
            $rd close
        }
    }

    start_server {tags {"zvset-compat"}} {
        r config set zset-max-listpack-entries $zv_max_entries
        test "ZVRANKBYSCORE and ZVREVRANKBYSCORE" {
            r del rk
            r zvadd rk 1#0#3 a 1#0#2 b 3#2#4 c 2#0#2 d
            # Order: b(1#0#2) a(1#0#3) d(2#0#2) c(3#2#4)
            assert_equal 2 [r zvrankbyscore rk 2#0#2]
            assert_equal 1 [r zvrevrankbyscore rk 2#0#2]
            assert_equal 0 [r zvrankbyscore rk 0#0#0]
            assert_equal 4 [r zvrankbyscore rk 9#9#9]
            assert_equal 0 [r zvrevrankbyscore rk 9#9#9]
            assert_equal 4 [r zvrevrankbyscore rk 0#0#0]
            assert_equal 0 [r zvrankbyscore rk-missing 1#2]
            assert_error "*dimension*" {r zvrankbyscore rk 1#2}
            assert_error "*invalid vector*" {r zvrankbyscore rk foo}
            r set rk-str x
            assert_error "*WRONGTYPE*" {r zvrankbyscore rk-str 1}
        }

        test "Scalar -inf/+inf bounds cover every dimension" {
            r del ik
            r zvadd ik 1#2 a 3#4 b -inf#-inf lo +inf#+inf hi
            assert_equal {lo a b hi} [r zvrangebyscore ik -inf +inf]
            assert_equal {hi b a lo} [r zvrevrangebyscore ik +inf -inf]
            assert_equal 4 [r zvcount ik -inf +inf]
            assert_equal {a b hi} [r zvrangebyscore ik (-inf +inf]
            assert_equal {hi} [r zvrangebyscore ik +inf +inf]
            assert_equal 2 [r zvremrangebyscore ik (-inf (+inf]
            assert_equal {lo hi} [r zvrange ik 0 -1]
        }

        test "ZVINCRBY treats every argument as an increment, not an option" {
            r del ik2
            assert_error "*invalid vector*" {r zvincrby ik2 nx m}
            assert_equal 0 [r exists ik2]
        }

        test "ZVSET keyspace notifications use the sorted set class and emit del" {
            set db [expr {$::singledb ? 0 : 9}]
            r config set notify-keyspace-events Kz
            set rd [valkey_deferring_client]
            $rd psubscribe __keyspace@*__:nk
            $rd read
            r zvadd nk 1#1 a 2#2 b
            r zvrem nk a b
            assert_equal "pmessage __keyspace@*__:nk __keyspace@${db}__:nk zvadd" [$rd read]
            assert_equal "pmessage __keyspace@*__:nk __keyspace@${db}__:nk zvrem" [$rd read]
            $rd close
            r config set notify-keyspace-events Kg
            set rd [valkey_deferring_client]
            $rd psubscribe __keyspace@*__:nk
            $rd read
            r zvadd nk 1#1 a
            r zvrem nk a
            assert_equal "pmessage __keyspace@*__:nk __keyspace@${db}__:nk del" [$rd read]
            $rd close
            r config set notify-keyspace-events ""
        }
    }

    start_server {tags {"zvset-query"}} {
        r config set zset-max-listpack-entries $zv_max_entries
        proc zv_setup_players {} {
            r del players
            r zvadd players 1000#20#5 alice 1100#25#7 bob 1200#15#9 carol 1300#30#5 dave
        }

        test "ZVQUERY basic AND filters" {
            zv_setup_players
            assert_equal {alice bob carol} [r zvquery players filter 0 1000 1200 filter 1 0 30]
            assert_equal {alice bob} [r zvquery players filter 0 1000 1100 filter 2 5 7]
            # no filters = everything in vector order
            assert_equal {alice bob carol dave} [r zvquery players]
            # withscores
            assert_equal {bob 1100#25#7} [r zvquery players filter 0 1100 1100 withscores]
        }

        test "ZVQUERY exclusive bounds and limits" {
            zv_setup_players
            assert_equal {carol} [r zvquery players filter 0 (1100 1200]
            assert_equal {alice bob} [r zvquery players filter 0 1000 (1200]
            assert_equal {carol dave} [r zvquery players filter 0 1000 + limit 2 10]
            assert_equal {dave} [r zvquery players filter 0 1000 + limit 3 10]
            assert_equal {} [r zvquery players filter 0 1000 + limit 0 0]
            assert_equal {bob carol} [r zvquery players filter 0 1000 1200 limit 1 -1]
        }

        test "ZVQUERY edge cases" {
            zv_setup_players
            # reversed range is empty
            assert_equal {} [r zvquery players filter 0 1200 1000]
            assert_equal {} [r zvquery players filter 0 1100 (1100]
            # dimension out of range
            assert_error "*out of range*" {r zvquery players filter 3 0 10}
            assert_error "*non-negative*" {r zvquery players filter -1 0 10}
            # bad bound
            assert_error "*invalid filter*" {r zvquery players filter 0 abc 10}
            assert_error "*invalid filter*" {r zvquery players filter 0 1#2 10}
            # missing key
            assert_equal {} [r zvquery nokey filter 0 0 10]
            # wrong type
            r set s str
            assert_error "*WRONGTYPE*" {r zvquery s filter 0 0 10}
        }

        test "ZVQUERY seeks across tied scores, binary members and infinities" {
            r del seek
            set binary "a\x00\xff"
            r zvadd seek -inf#0 low -1#0 neg 0#-inf zero-low -0#inf $binary 1#0 one inf#0 high
            assert_equal [list zero-low $binary] [r zvquery seek filter 0 -0 0]
            assert_equal {one high} [r zvquery seek filter 0 (0 +]
            assert_equal {low neg} [r zvquery seek filter 0 - (0]
            assert_equal {low} [r zvquery seek filter 0 -inf -inf]
            assert_equal {high} [r zvquery seek filter 0 inf inf]
            assert_equal {} [r zvquery seek filter 0 (inf +]
            assert_equal {} [r zvquery seek filter 0 - (-inf]
            assert_equal {neg zero-low} [r zvquery seek filter 0 -1 1 limit 0 2]
            assert_equal [list $binary one] [r zvquery seek filter 0 -1 1 limit 2 -1]
            assert_equal {} [r zvquery seek filter 0 2 3]
            assert_equal {} [r zvquery seek filter 0 -3 -2]
            assert_equal {} [r zvquery seek filter 0 0 + filter 0 - (0]
            assert_equal {one} [r zvquery seek filter 0 -1 1 filter 0 (0 inf]
            assert_equal {one} [r zvquery seek filter 0 (0 inf filter 0 -1 1]
            assert_error "*out of range*" {r zvquery seek filter 2 0 1 limit 0 0}
        }

        test "ZVQUERY indexed bounds agree with a full scan over multiple tree leaves" {
            r del query-many
            for {set i 0} {$i < 2000} {incr i} {
                r zvadd query-many "[expr {($i * 37) % 101 - 50}]#[expr {$i % 17}]" m$i
            }
            set all [r zvrange query-many 0 -1 withscores]
            foreach min {-50 -7 0 13 50 51} {
                foreach max {-51 -7 0 13 50} {
                    foreach exclusive {0 1} {
                        set expected {}
                        set first_only {}
                        foreach {member score} $all {
                            lassign [split $score #] first second
                            if {($exclusive ? $first > $min : $first >= $min) && $first <= $max} {
                                lappend first_only $member
                                if {$second >= 3 && $second < 11} {lappend expected $member}
                            }
                        }
                        set lower [expr {$exclusive ? "($min" : "$min"}]
                        assert_equal $expected [r zvquery query-many filter 1 3 (11 filter 0 $lower $max]
                        assert_equal [lrange $expected 3 9] [r zvquery query-many filter 0 $lower $max filter 1 3 (11 limit 3 7]
                        assert_equal [lrange $first_only 3 9] [r zvquery query-many filter 0 $lower $max limit 3 7]
                    }
                }
            }
            set members [r zvrange query-many 0 -1]
            assert_equal [lrange $members 1995 end] [r zvquery query-many limit 1995 -1]
            assert_equal {} [r zvquery query-many limit 2000000000 10]
        }

        test "ZV set operations use packed lookup keys with different scores" {
            r del packed-a packed-b packed-result
            set members [list "" "a\x00\xff" [string repeat x 300]]
            foreach member $members {
                r zvadd packed-a 1#2 $member
                r zvadd packed-b 3#4 $member
            }
            r zvadd packed-a 5#6 only-a
            r zvadd packed-b 7#8 only-b
            assert_equal 3 [r zvintercard 2 packed-a packed-b]
            assert_equal 2 [r zvintercard 2 packed-a packed-b limit 2]
            assert_equal {only-a} [r zvdiff 2 packed-a packed-b]
            assert_equal 3 [r zvinterstore packed-result 2 packed-a packed-b]
            foreach member $members {
                assert_equal 4#6 [r zvscore packed-result $member]
            }
            assert_equal 5 [r zvunionstore packed-result 2 packed-a packed-b]
            foreach member $members {
                assert_equal 4#6 [r zvscore packed-result $member]
            }
        }

        test "ZV maximum dimension score formatting and late GT LT comparison" {
            r del format-max
            set values {}
            set extremes {-inf -1.7976931348623157e308 -0 2.2250738585072014e-308 1.7976931348623157e308 inf}
            for {set i 0} {$i < 255} {incr i} {
                lappend values [lindex $extremes [expr {$i % [llength $extremes]}]]
            }
            set vector [join $values #]
            r zvadd format-max $vector extreme
            set formatted [r zvscore format-max extreme]
            set decoded [split $formatted #]
            assert_equal 255 [llength $decoded]
            foreach expected $values actual $decoded {
                assert_equal [expr {double($expected)}] [expr {double($actual)}]
            }
            assert_equal $formatted [r zvincrby format-max [join [lrepeat 255 0] #] extreme]
            assert_equal 0 [r zvadd format-max CH $formatted extreme]

            set prefix [lrepeat 254 1]
            set small [join [concat $prefix 2] #]
            set large [join [concat $prefix 3] #]
            r zvadd format-max $small late
            assert_equal 1 [r zvadd format-max GT CH $large late]
            assert_equal 0 [r zvadd format-max GT CH $small late]
            assert_equal 1 [r zvadd format-max LT CH $small late]
            assert_equal 0 [r zvadd format-max LT CH $large late]
            assert_equal $small [r zvscore format-max late]
        }
    }
}

start_server {tags {"zvset-encoding needs:debug"}} {
    test "ZVSET compact encoding converts to btree past zset-max-listpack-entries" {
        r config set zset-max-listpack-entries 4
        r del k
        r zvadd k 1 a 2 b 3 c 4 d
        assert_encoding compact k
        r zvadd k 5 e
        assert_encoding btree k
        assert_equal {a b c d e} [r zvrange k 0 -1]
        # Deleting never converts back in place.
        r zvrem k a b c
        assert_encoding btree k
        assert_equal {d e} [r zvrange k 0 -1]
        r config set zset-max-listpack-entries 128
    }

    test "ZVSET member longer than zset-max-listpack-value converts to btree" {
        r config set zset-max-listpack-value 8
        r del k
        r zvadd k 1 short
        assert_encoding compact k
        r zvincrby k 1 [string repeat x 9]
        assert_encoding btree k
        assert_equal [list short [string repeat x 9]] [r zvrange k 0 -1]
        r config set zset-max-listpack-value 64
    }

    test "ZVSET STORE results start in the encoding that fits" {
        r config set zset-max-listpack-entries 4
        r del big1{t} big2{t} dst{t}
        for {set i 0} {$i < 10} {incr i} {
            r zvadd big1{t} $i#0 m$i
        }
        r zvadd big2{t} 1#1 m1 2#2 m2 100#0 x1 101#0 x2 102#0 x3 103#0 x4
        assert_encoding btree big1{t}
        assert_equal 2 [r zvinterstore dst{t} 2 big1{t} big2{t}]
        assert_encoding compact dst{t}
        assert_equal {m1 m2} [r zvrange dst{t} 0 -1]
        assert_equal 14 [r zvunionstore dst{t} 2 big1{t} big2{t}]
        assert_encoding btree dst{t}
    }

    test "ZVSET RANGESTORE and DIFFSTORE pick the encoding by result size" {
        r config set zset-max-listpack-entries 4
        assert_equal 2 [r zvrangestore dst{t} big1{t} 0 1]
        assert_encoding compact dst{t}
        assert_equal {m0 m1} [r zvrange dst{t} 0 -1]
        assert_equal 8 [r zvdiffstore dst{t} 2 big1{t} big2{t}]
        assert_encoding btree dst{t}
        assert_equal 4 [r zvdiffstore dst{t} 2 big2{t} big1{t}]
        assert_encoding compact dst{t}
        assert_equal {x1 x2 x3 x4} [r zvrange dst{t} 0 -1]
        r config set zset-max-listpack-entries 128
    }

    test "ZVSET STORE results built in bulk keep vector order" {
        # Enough members for the radix sort, with many ties on the first
        # component (including infinities and negatives) and on the full
        # vector, so ties fall back to the second component and the member.
        r del a{t} b{t} dst{t}
        set firsts {-inf -1.5 0 2 inf}
        set ref_a {}
        set ref_b {}
        for {set i 0} {$i < 400} {incr i} {
            set v [list [lindex $firsts [expr {$i % 5}]] [expr {($i * 37) % 11 - 5}]]
            r zvadd a{t} [join $v #] m$i
            dict set ref_a m$i $v
            if {$i % 2} {
                r zvadd b{t} 1#1 m$i
                dict set ref_b m$i {1 1}
            }
        }
        assert_equal 400 [r zvunionstore dst{t} 2 a{t} b{t} aggregate min]
        assert_encoding btree dst{t}
        set ref_min {}
        dict for {m v} $ref_a {
            if {[dict exists $ref_b $m] && [zv_compare [concat x {1 1}] [concat x $v]] < 0} {
                dict set ref_min $m {1 1}
            } else {
                dict set ref_min $m $v
            }
        }
        zv_check dst{t} $ref_min
        assert_equal 200 [r zvdiffstore dst{t} 2 a{t} b{t}]
        zv_check dst{t} [dict filter $ref_a script {m v} {expr {![dict exists $ref_b $m]}}]
        assert_equal 200 [r zvinterstore dst{t} 2 a{t} b{t} weights 1 0]
        zv_check dst{t} [dict filter $ref_a script {m v} {dict exists $ref_b $m}]
    }

    test "ZVSET encodings survive DEBUG RELOAD and COPY" {
        r config set zset-max-listpack-entries 4
        r del small{t} large{t} c1{t} c2{t}
        r zvadd small{t} 2#1 b 1#1 a
        for {set i 0} {$i < 10} {incr i} {
            r zvadd large{t} $i#$i m$i
        }
        r copy small{t} c1{t}
        r copy large{t} c2{t}
        assert_encoding compact c1{t}
        assert_encoding btree c2{t}
        set small [r zvrange small{t} 0 -1 withscores]
        set large [r zvrange large{t} 0 -1 withscores]
        r debug reload
        assert_encoding compact small{t}
        assert_encoding btree large{t}
        assert_equal $small [r zvrange small{t} 0 -1 withscores]
        assert_equal $large [r zvrange large{t} 0 -1 withscores]
        assert_equal $small [r zvrange c1{t} 0 -1 withscores]
        assert_equal $large [r zvrange c2{t} 0 -1 withscores]
        r config set zset-max-listpack-entries 128
    }

    test "ZVSET compact and btree agree under random operations" {
        set seed [expr {int(rand() * 1000000)}]
        puts "zv encoding fuzz seed: $seed"
        expr {srand($seed)}
        # b{e} is created as btree and stays btree; c{e} stays compact
        # because it never exceeds 128 members.
        proc zv_fuzz_reset {} {
            r del c{e} b{e}
            r config set zset-max-listpack-entries 0
            r zvadd b{e} 0#0 seed
            r config set zset-max-listpack-entries 128
            r zvadd c{e} 0#0 seed
        }
        zv_fuzz_reset
        assert_encoding btree b{e}
        assert_encoding compact c{e}
        proc zv_rand_score {} {
            set kind [expr {int(rand() * 10)}]
            if {$kind == 0} {return "-inf#[expr {int(rand() * 5)}]"}
            if {$kind == 1} {return "inf#[expr {int(rand() * 5)}]"}
            return "[expr {int(rand() * 8)}]#[expr {int(rand() * 8)}]"
        }
        for {set i 0} {$i < 2000} {incr i} {
            set m "m[expr {int(rand() * 100)}]"
            set op [expr {int(rand() * 10)}]
            set cmd {}
            switch $op {
                0 - 1 - 2 { set cmd [list zvadd KEY [zv_rand_score] $m] }
                3 { set cmd [list zvadd KEY GT [zv_rand_score] $m] }
                4 { set cmd [list zvincrby KEY [zv_rand_score] $m] }
                5 { set cmd [list zvrem KEY $m] }
                6 { set cmd [list zvpopmin KEY] }
                7 { set cmd [list zvremrangebyscore KEY "([zv_rand_score]" [zv_rand_score]] }
                8 { set cmd [list zvremrangebyrank KEY 3 4] }
                9 { set cmd [list zvadd KEY 0#0 seed] }
            }
            set rc [catch {r {*}[lreplace $cmd 1 1 c{e}]} resc]
            set rb [catch {r {*}[lreplace $cmd 1 1 b{e}]} resb]
            assert_equal [list $rb $resb] [list $rc $resc]
            if {![r exists b{e}]} {
                assert_equal 0 [r exists c{e}]
                zv_fuzz_reset
            }
            if {$i % 50 == 0} {
                assert_equal [r zvrange b{e} 0 -1 withscores] [r zvrange c{e} 0 -1 withscores]
                set lo [zv_rand_score]
                set hi [zv_rand_score]
                foreach q [list \
                    [list zvcount KEY $lo $hi] \
                    [list zvcount KEY ($lo +] \
                    [list zvrangebyscore KEY $lo $hi withscores limit 1 3] \
                    [list zvrevrangebyscore KEY $hi $lo] \
                    [list zvrank KEY $m withscore] \
                    [list zvrevrank KEY $m] \
                    [list zvrankbyscore KEY $lo] \
                    [list zvrevrankbyscore KEY $hi] \
                    [list zvrange KEY 2 -2 rev withscores]] {
                    assert_equal [r {*}[lreplace $q 1 1 b{e}]] [r {*}[lreplace $q 1 1 c{e}]]
                }
                # SCAN order differs between encodings.
                set sb [r zvscan b{e} 0 count 1000]
                set sc [r zvscan c{e} 0 count 1000]
                assert_equal 0 [lindex $sb 0]
                assert_equal 0 [lindex $sc 0]
                assert_equal [lsort -stride 2 [lindex $sb 1]] [lsort -stride 2 [lindex $sc 1]]
            }
        }
        assert_encoding compact c{e}
        assert_encoding btree b{e}
        assert_equal [r debug digest-value b{e}] [r debug digest-value c{e}]
    }
}
