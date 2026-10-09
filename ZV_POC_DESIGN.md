# ZV PoC DESIGNメモ

ZV (Vector Sorted Set) PoC: native multi-score sorted set。
1 memberが複数の `double` scoreを持ち、score vectorを左から辞書順比較する。

<!-- SPDX-License-Identifier: BSD-3-Clause -->

## 構成

既存 ZSET / `OrderedIndex` は無変更。ZVは `fbtree` を直接利用する。

```text
              ZSET
                |
         OrderedIndex
                |
                v
             fbtree
                ^
                |
              ZVSET
```

## packed item layout

1要素=1 SDS:

```text
+------+----------+----------+-----+----------+--------+
| dims | score[0] | score[1] | ... | score[n] | member |
+------+----------+----------+-----+----------+--------+
   1B       8B         8B              8B      variable
```

- 各scoreは `ordered_index.c` の `scoreToSortable()` と同一変換の
  sortable big-endian u64 (`zvScoreToSortable()` in `src/zvset.c`)。
  PoC指示通り共通化せずZV側に複製。
- 同一keyでは `dims` が全itemで同一のため先頭1Bは順序に影響しない。

## ordering rule

`score[0]` → 同値なら `score[1]` → … → 全同値なら member byte lexicographical。
`fbtree` はSDSを `sdscmp` (byte lexicographical) で比較するため、
packed itemの素朴な挿入だけでvector lexicographic orderingが成立する。
comparator等のfbtree側変更はなし (`fbtree.c`, `ordered_index.*` 無変更)。

## doubleの規則

- `NaN` 拒否 (`string2d()` が拒否)。
- `-0.0` は `+0.0` にcanonicalize (parse時・pack時・sortable変換時の3箇所)。
- `+inf` / `-inf` はZSET同様に扱う。

## score parser

protocol representationは `1#2#3.5` 形式 (`#` 区切り、TairZset互換)。
`zvScoreParse()` / `zvScoreFree()` (`src/zvset.c`) に隔離し、
storage representationと分離。将来的なprotocol変更はparser差し替えで対応。

- `ZVADD` は全pairをparse・validateしてからmutate (atomicity)。
- dimensionは `1 <= d <= 255` (`uint8_t`)。

## ownership rule

ZSET(BTREE)と同じ設計。hashtable entryはpacked fbtree itemへの
non-owning pointer (同一pointer)。

```text
             +------------------+
             |      fbtree      |  ← packed SDS itemを所有
             +--------+---------+
                      | same pointer
             +--------v---------+
             |    hashtable     |  ← destructorなし
             +------------------+
```

- member lookupはplain SDS + `zsetMarkLookupKey()` marking reuse。
  extractorはmark有無でplain SDSかpacked itemかを判別
  (`zvsetExtractElement()` in `src/zvset.c`)。
- free順: `hashtableRelease()` → `fbtreeFree()` (item実体解放) → `zfree(zs)`。
- update順: new SDS作成 → oldを `fbtreeDelete` → newを `fbtreeInsert` →
  hashtable bucket内pointer置換。
- delete順: `hashtablePop` → `fbtreeDelete` (逆にするとuse-after-free)。

## dimension invariant

key単位で固定。最初の `ZVADD` で確定し、以降の不一致は
`vector dimension mismatch for existing key` エラー。
単一 `ZVADD` 内の混在も `all vector scores must have the same dimension`
エラーでatomicに拒否 (部分的適用なし)。

## fbtree API mapping

| ZV操作     | fbtree API                                              |
|------------|---------------------------------------------------------|
| ADD insert | `fbtreeInsert()` → `hashtableAdd()`                     |
| ADD update | `fbtreeDelete(old)` → `fbtreeInsert(new)` → slot更新    |
| REM        | `hashtablePop()` → `fbtreeDelete()`                     |
| SCORE      | hashtable lookupのみ (treeを触らない)                   |
| CARD       | `hashtableSize()` (debug assertでtree長と一致確認)      |
| RANK       | hashtable lookup → `fbtreeGetIndexOfItem()`             |
| RANGE      | `fbtreeInitIterator()` + `fbtreeSeekToRank()` + `fbtreeNext()` |
| COPY       | 降順 `fbtreePrev()` walk + `fbtreeInsert`               |
| RDB save   | hashtable walk → member + binary doubles                |
| AOF rewrite| `ZVADD` 再発行 (`rewriteZvsetObject()`)                 |
| defrag     | `fbtreeDefragScan()` + hashtable pointer更新callback    |

計算量目標: `ZVADD O(d·logN)` (worst; tree探索中の各キー比較がvector長に
依存するため。fbtreeはprefix特徴量を活用するので常に最悪になるわけではない),
`ZVREM O(logN)`, `ZVSCORE O(1+d)`,
`ZVRANK O(logN)`, `ZVRANGE O(logN+M)`, `ZVCARD O(1)`。
`d` 依存の定量化には先頭次元が同一のworkload B (`shared`) 測定が重要。

## 変更ファイル

- 新規: `src/zvset.h`, `src/zvset.c`, `src/t_zvset.c`,
  `src/commands/zv{add,rem,score,rank,range,card}.json`,
  `tests/unit/type/zvset.tcl`, `utils/zv_benchmark.tcl`,
  `.github/workflows/zv-poc.yml`
- 既存(最小限の配線のみ): `src/server.h` (`OBJ_ZVSET=8`, `OBJ_TYPE_MAX=9`,
  prototypes), `src/object.c`, `src/db.c` (type名/COPY),
  `src/rdb.h`/`src/rdb.c` (`RDB_TYPE_ZVSET=24`), `src/aof.c`,
  `src/lazyfree.c`, `src/defrag.c`, `src/debug.c`,
  `src/valkey-check-rdb.c`, `src/Makefile`,
  `cmake/Modules/SourceFiles.cmake`, `src/commands.def` (生成),
  push時非テストGHA6件に `branches-ignore: poc/zv-multiscore`。
- 無変更: `src/fbtree.c`, `src/ordered_index.c`, `src/ordered_index.h`,
  ZSETのsemantics/storage。

## benchmark結果 (baseline, N=100k, loopback, dev機・参考値)

`utils/zv_benchmark.tcl` (--n 100000 --dims "1 2 4 8 16"
--dists "spread shared")。値はbatch中央値ops/sec。
`spread` は先頭次元で決着しやすい分布、`shared` は最終次元まで
比較する分布 (workload B)。

spread:

| test               | d1     | d2    | d4    | d8    | d16   | ZSET  |
|--------------------|--------|-------|-------|-------|-------|-------|
| ZVADD insert o/s   | 893k   | 849k  | 713k  | 669k  | 568k  | 847k  |
| ZVADD update o/s   | 811k   | 742k  | 690k  | 633k  | 509k  | 824k  |
| ZVRANK o/s         | 840k   | —     | —     | —     | 781k  | 935k  |
| ZVSCORE o/s        | 645k   | —     | —     | —     | 465k  | 862k  |
| ZVRANGE 0-99 o/s   | 14320  | —     | —     | —     | 14045 | 14148 |
| RANGE+scores o/s   | 7049   | —     | —     | —     | 3880  | 6769  |
| MEMORY (100k)      | 4.75MB | 5.55MB| 7.15MB| 10.35MB|18.35MB| 4.75MB|

shared (workload B):

| test               | d1    | d2    | d4    | d8    | d16   |
|--------------------|-------|-------|-------|-------|-------|
| ZVADD insert o/s   | 783k  | 882k  | 818k  | 726k  | 603k  |
| ZVADD update o/s   | 817k  | 781k  | 731k  | 646k  | 538k  |
| MEMORY (100k)      | 4.75MB| 5.86MB| 7.37MB| 10.44MB|18.86MB|

読み方:

- d1のZVはZSETと同等 (insert 893k vs 847k)。tree自体にdimension依存処理なし。
- d1→d16でinsert約0.64倍 (spread/sharedとも)。劣化は主に
  encoding/decoding・packed key size・byte比較・memory bandwidth。
  shared分布でも同程度であり、最悪寄りワークロードでも崩れない。
- MEMORYは `1+8d` B/memberのリニア増加 + fbtree overhead。
- p50/p99はloopback RTT参考値 (CI artifactのTSVに記録)。

再現: `tclsh utils/zv_benchmark.tcl --port <port> --n 1000000 --dims "1 2 4 8 16"`、
またはGHA `zv-poc.yml` をworkflow_dispatch (n=1000000) で実行。

## CI整理

- pushで動く非テスト系は `poc/zv-multiscore` で無効化
  (`branches-ignore`): clang-format, codecov, codeql,
  reply-schemas-linter, reuse, spell-check。
- テスト系は継続: `ci.yml`, `external.yml`。
- 新規 `.github/workflows/zv-poc.yml`:
  - `zv-test`: build + `unit/type/zvset` + `unit/type/zset` 回帰。
  - `zv-bench`: `utils/zv_benchmark.tcl` 実行、TSV/txtをartifact化し
    summaryに投稿。push時はN=20000のquick版、
    workflow_dispatchでN指定 (default 1000000)。

## PoC未対応項目

`ZVINCRBY`, `GT`/`LT`, `ZVCOUNT`, `ZVRANGE BYSCORE`, `REV`,
`ZVPOPMIN`/`MAX`, `ZVUNION`/`INTER`/`DIFF`, `SCAN`, blocking,
listpack/compact encoding, `COPY` 以外の型横断対応の深掘り,
active defragの本格検証, module API, cluster固有integration,
per-dimension ASC/DESC, 多次元filter, secondary index。

RDB/AOFはPoC最小実装 (`RDB_TYPE_ZVSET`, `ZVADD` 再発行)。
`DEBUG RELOAD` および `COPY` はテスト済み。

注意: `clang-format-18` が手元環境になく未適用。
upstream提出前に `clang-format-18 -i` が必要。
