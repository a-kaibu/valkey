# ZV DESIGNメモ (full-command PoC)

ZV (Vector Sorted Set): native multi-score sorted set。
1 memberが複数の `double` scoreを持ち、score vectorを左から辞書順比較する。
TairZset相当以上の機能をValkey 9.2のZSETに近い統一APIで提供する。

<!-- SPDX-License-Identifier: BSD-3-Clause -->

## コマンド一覧 (35コマンド名)

基本: `ZVADD` (NX/XX/GT/LT/CH/INCR), `ZVREM`, `ZVSCORE`, `ZVMSCORE`,
`ZVRANK`/`ZVREVRANK` (WITHSCORE), `ZVRANKBYSCORE`/`ZVREVRANKBYSCORE`
(TairZset `EXZRANKBYSCORE` 相当), `ZVCARD`, `ZVINCRBY`。

範囲: `ZVRANGE` (rank/BYSCORE/BYLEX + REV/LIMIT/WITHSCORES),
`ZVREVRANGE`, `ZVRANGEBYSCORE`, `ZVREVRANGEBYSCORE`,
`ZVRANGEBYLEX`, `ZVREVRANGEBYLEX`, `ZVCOUNT`, `ZVLEXCOUNT`,
`ZVRANGESTORE`, `ZVREMRANGEBYRANK`, `ZVREMRANGEBYSCORE`, `ZVREMRANGEBYLEX`。
互換コマンドは共通coreへの薄いラッパー。

POP: `ZVPOPMIN`, `ZVPOPMAX`, `ZVMPOP`, `BZVPOPMIN`, `BZVPOPMAX`, `BZVMPOP`。

集合演算: `ZVUNION`, `ZVUNIONSTORE`, `ZVINTER`, `ZVINTERSTORE`,
`ZVDIFF`, `ZVDIFFSTORE`, `ZVINTERCARD` (SUM=成分加算, MIN/MAX=vector全体比較,
NaN成分は0へ正規化)。

SCAN: `ZVSCAN` (汎用SCAN経路, MATCH/COUNT, member+vector返却)。

実験的: `ZVQUERY` (FILTER dim min maxのAND、先頭次元はtree range seek、LIMIT/WITHSCORES)。

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
- 範囲境界のスカラー `-inf` / `+inf` は多次元キーでも全次元がその値の
  vectorに展開する (TairZsetの `EXZRANGEBYSCORE key -inf +inf` 互換)。

## score parser

protocol representationは `1#2#3.5` 形式 (`#` 区切り、TairZset互換)。
`zvScoreParse()` / `zvScoreFree()` (`src/zvset.c`) に隔離し、
storage representationと分離。将来的なprotocol変更はparser差し替えで対応。

- `ZVADD` は全pairをparse・validateしてからmutate (atomicity)。
- dimensionは `1 <= d <= 255` (`uint8_t`)。

## encoding (compact / btree)

`zvset` 構造体が自分の表現を持つ (`zvsetIsCompact()`、robjのencodingは常に
`OBJ_ENCODING_BTREE`)。`OBJECT ENCODING` は `compact` / `btree` を返す。

- compact: packed itemのソート済み配列 + memberごとの16bitハッシュタグ
  (1 allocationに `[sds × capacity][uint16 tag × capacity]`)。hashtable・
  fbtreeなし。rank系はbinary search、member lookupはtag配列を線形走査し
  tag一致時だけitemを参照。
- 閾値はZSETのlistpackと同じ `zset-max-listpack-entries` (128) /
  `zset-max-listpack-value` (64)。超えた時点でbtreeへ変換 (item pointerは
  そのまま移すのでcopyなし)。in placeでの逆変換はしない。
- STORE系 (`ZVRANGESTORE`, `ZVUNIONSTORE` 等) とRDB loadは結果サイズで
  最初からencodingを選ぶ。INTER/DIFFSTOREは結果が小さければcompactへ戻す。
- encoding差は `src/zvset.c` の container API (`zvsetFind`, `zvsetRankOf`,
  `zvsetGetAtRank`, `zvsetPop`, `zvsetDeleteRangeByRank`, `zvsetIterator`
  など) に閉じており、t_zvset.c・rdb・aof・digest・scanはencodingを見ない
  (SCANとdefragのみcompact用の分岐あり)。
- テストは `tests/unit/type/zvset.tcl` 全体を両encodingで実行し、
  compact/btreeの差分ファジングも行う。

## ownership rule (btree)

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
| RANGE rank | `fbtreeSeekToRank()` + `fbtreeNext()`/`fbtreePrev()`    |
| BYSCORE境界 | `fbtreeSeekToValue()` ×2 → `[lo,hi)` rank              |
| COUNT      | `fbtreeCountRangeByValue(lo,hi,0,1)` (両 bounded) / rank差 |
| BYLEX      | uniform判定 (`fbtreePeekMin/Max` prefix比較) 後 Value系 |
| 範囲削除   | `fbtreeDeleteRangeByRank/Value` + ht-sync callback      |
| POP        | `fbtreePopMin/Max` (所有権移転) → ht削除 → free         |
| RANDMEMBER | `fbtreeGetAtRank()` (sparse Fisher-Yates)               |
| SCAN       | hashtable走査 (`scanGenericCommand` 拡張)                |
| RANGESTORE | `zvIterateRange()` 収集 → 新規build → 置換              |
| QUERY      | 先頭次元のtree range seek + `zvItemScoreAt()` filter     |
| COPY       | 降順 `fbtreePrev()` walk + `fbtreeInsert`               |
| RDB save   | hashtable walk → member + binary doubles                |
| AOF rewrite| `ZVADD` 再発行 (`rewriteZvsetObject()`)                 |
| defrag     | `fbtreeDefragScan()` + hashtable pointer更新callback    |
| blocking   | 専用の `BLOCKED_ZVSET`。ZSETと共用するとZVADDでBZPOPMIN |
|            | 待ちが起床してWRONGTYPEになるため分離 (ZSET作成でも      |
|            | BZVPOP*は起床しない)。                                  |
| 範囲解決   | rank/score/lex を `zvResolveRange()` で rank窓 [lo,hi)  |
|            | に変換し、RANGE*/RANGESTORE/COUNT/REMRANGEBY* が共用。  |

計算量目標: `ZVADD O(d·logN)` (worst; tree探索中の各キー比較がvector長に
依存するため。fbtreeはprefix特徴量を活用するので常に最悪になるわけではない),
`ZVREM O(logN)`, `ZVSCORE O(1+d)`,
`ZVRANK O(logN)`, `ZVRANGE O(logN+M)`, `ZVCARD O(1)`。
`d` 依存の定量化には先頭次元が同一のworkload B (`shared`) 測定が重要。

## 変更ファイル

- 新規: `src/zvset.h`, `src/zvset.c`, `src/t_zvset.c`,
  `src/commands/zv*.json` (40種: ZV 33名 + 互換分),
  `tests/unit/type/zvset.tcl` (55+ tests, randomized fuzz),
  `utils/zv_bench_lib.tcl`, `utils/zv_{benchmark,range,pop,setops,query}_benchmark.tcl`,
  `.github/workflows/zv-poc.yml`
- 既存(最小限の配線のみ): `src/server.h` (`OBJ_ZVSET=8`, `OBJ_TYPE_MAX=9`,
  prototypes), `src/object.c`, `src/db.c` (type名/COPY/SCAN汎用経路),
  `src/rdb.h`/`src/rdb.c` (`RDB_TYPE_ZVSET=24`), `src/aof.c`,
  `src/lazyfree.c`, `src/defrag.c`, `src/debug.c`,
  `src/valkey-check-rdb.c`, `src/blocked.c`/`src/cluster.c` (`BLOCKED_ZVSET` の追加),
  `src/Makefile`, `cmake/Modules/SourceFiles.cmake`,
  `src/commands.def` (生成),
  push時非テストGHA6件に `branches-ignore: poc/zv-multiscore`。
- 無変更: `src/fbtree.c`, `src/ordered_index.c`, `src/ordered_index.h`,
  ZSETのsemantics/storage。
- `clang-format-18` 適用済み。

## benchmark結果

基本 (`utils/zv_benchmark.tcl`, N=100k, loopback, dev機・参考値。
batch中央値。`spread`=先頭決着, `shared`=最終次元まで比較):

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

shared (workload B): insert 783k→603k, update 817k→538k (d1→d16)。
最悪寄り分布でも同程度の劣化率であり、崩れない。

範囲 (`utils/zv_range_benchmark.tcl`, N=100k, d4/d16):

- `RANGE-M1` 470k o/s → `M10000` 126 o/s: `O(logN+M)` のうち
  M≥100では結果出力が支配的。WITHSCORESは約1/2〜1/3。
- `BYSCORE-narrow` (M≈200): spread 240k o/s。`COUNT` はMに依らず
  480k〜1.3M o/s (narrowはValue-count, wideはrank差分)。
- `ZVREMRANGEBYSCORE-all` (100k件): 単発で完結、bulk削除はtree走査
  1回＋callbackでht同期。
- narrow測定の注意: lex順では先行次元の`-inf`/`+inf`やeps-boxが
  後続次元の絞り込みを無効化する。真のnarrowはcenter固定＋
  最終次元のみ+1.0のsliceで取る (MをZVCOUNTで記録)。

POP (`utils/zv_pop_benchmark.tcl`, N=100k):
`ZVPOPMIN-C1` 15k pops/s → `C1000` 300k pops/s (COUNT amortization)。
`ZVMPOP-C100` 172k o/s。`BZVPOPMIN` wake-to-pop ≈ 376µs (loopback参考)。

集合演算 (`utils/zv_setops_benchmark.tcl`, N=100k/key, d4):
K=4/ov=50で `UNIONSTORE` 210ms/25万件、mem-delta約19MB。
`ZVINTERCARD` full 42.8ms → `LIMIT 10` で0.1ms (early exitの効果)。
重複率0→100%でINTERはdriver走査量が支配的。

多次元検索 (`utils/zv_query_benchmark.tcl`, N=100k, full-scan):
selectivity 100%→0.01%で18ms→1ms。時間の大半はN件走査ではなく
返却行数 (出力支配)。filter追加でMが幾何級数的に減少し時間も追随。
dim-first/lastで大差なし (d16: 1.8ms vs 4.1ms、要再測定)。

読み方:

- d1のZVはZSETと同等。tree自体にdimension依存処理なし。
- d1→d16でinsert約0.64倍 (spread/sharedとも)。劣化は主に
  encoding/decoding・packed key size・byte比較・memory bandwidth。
- MEMORYは `1+8d` B/memberのリニア増加 + fbtree overhead。
- p50/p99はloopback RTT参考値。TSVに加え `--json` でJSONL
  (commit/dims/dist/op/ops/p50/p99/memory) を出す。
- ボトルネック候補: 大M出力のRESP生成、集合演算の一時decode
  (member単位のmalloc)、`ZVQUERY` のN件走査 (secondary indexなし)。

再現: 各 `utils/zv_*_benchmark.tcl --port <port> --n ...`、
またはGHA `zv-poc.yml` (pushでbasic full、dispatch `bench=all` で全種)。

## CI整理

- pushで動く非テスト系は `poc/zv-multiscore` で無効化
  (`branches-ignore`): clang-format, codecov, codeql,
  reply-schemas-linter, reuse, spell-check。
- テスト系は継続: `ci.yml`, `external.yml`。
- 新規 `.github/workflows/zv-poc.yml` (追加pushでキャンセルされない
  `cancel-in-progress: false`):
  - `zv-test`: build + `unit/type/zvset` + `unit/type/zset` 回帰。
  - `zv-asan`: ASanビルド + `unit/type/zvset`。
  - `zv-bench` (timeout 360分): `utils/zv_benchmark.tcl` をフル条件
    (N=1000000, dims 1/2/4/8/16, spread+shared) で実行。
    push・workflow_dispatchのどちらでもフル計測し、
    TSV/JSONL/txtをartifact化しsummaryに投稿。
    dispatch `bench=all` で range/pop/setops/query も実行 (N cap 100K)。
  - `zv-ab`: base (pushでは直前のhead、dispatchでは `ab_base`) と
    HEAD を同一ランナーでビルドし、`utils/zv_cpu_benchmark.tcl`
    (N=30000, d16) を交互に3ラウンド実行。`INFO commandstats` の
    サーバー実行時間の中央値を比較表としてsummaryに投稿。

## 残課題 (upstream提案時)

実装済みのため当初のPoC未対応項目はほぼ解消。残るもの:

- per-dimension ASC/DESC、多次元filterのsecondary index
  (`ZVQUERY` は先頭次元の範囲を既存treeで絞り込む)。
- `RDB_TYPE_ZVSET=24` の正式type ID・RDB version対応の決定
  (現状RDB_VERSION 81のまま追加しており、素の9.2はこのRDBを読めない)。
- 最大次元数: TairZsetは256、本実装は先頭1Bに格納するため255。
- module API、cluster固有integrationの深掘り。
- `unstable` への追随。
- 本格性能評価は専用server/client・固定flags・5回中央値・
  p50/p95/p99/p99.9・Open Modelで別途 (現状はTcl簡易bench)。

## 最終報告 (§18対応)

1. 実装コマンド: 上記35コマンド名 (ラッパー含む)。
2. アーキテクチャ変更: ZVSET型追加＋既存経路への最小配線のみ。
   `fbtree.c`/`OrderedIndex`/ZSET無変更。
3. テスト: `unit/type/zvset` 55 passed (randomized fuzz含む)、
   `unit/type/zset` 364 passed、ASanで全ZVテスト＋大規模stressクリーン。
4. benchmark: 上記。d1でZSET同等、d16で約0.6倍。
   `ZVUNION/INTER` は入力規模にほぼリニア、`ZVINTERCARD LIMIT` は
   early exitで2桁高速、`ZVQUERY` は出力支配。
5. ボトルネック: 大M出力のRESP生成、集合演算の一時decode、
   full-scanのN件走査。
6. 残課題: 前節の通り。データ構造の作り込みより、正しさの保証と
   ZSET/TairZset比較の実測継続を推奨。

## 2026-10-11 local tuning measurements

Baseline: `97a4e070e`; candidate: working tree. Ryzen 5 7545U, Linux x86-64,
`-O3 -flto=auto`, jemalloc, persistence disabled, separate loopback servers.
`utils/zv_cpu_benchmark.tcl --n 30000 --dims 16 --repeats 7` reports medians
of `INFO commandstats` execution timers separately from client wall time.

| Case | Baseline server µs | Candidate server µs | Server speedup | Client wall speedup |
|---|---:|---:|---:|---:|
| QUERY FILTER 0 50 50.01 | 107.88 | 0.56 | 192.6× | 27.2× |
| QUERY LIMIT 10 | 42.41 | 0.81 | 52.4× | 4.3× |
| QUERY offset 20000 | 43.71 | 0.65 | 67.2× | 4.0× |
| QUERY last dimension | 246.56 | 207.97 | 1.19× | 1.08× |
| RANGE 100 WITHSCORES | 96.80 | 64.54 | 1.50× | 1.16× |
| UNIONSTORE 4 inputs, SUM | 56914.6 | 19856.6 | 2.87× | 2.86× |
| DIFFSTORE 2 identical member sets | 1544.4 | 1057.6 | 1.46× | 1.44× |

UNION aggregates packed items in its temporary hashtable before inserting each
final item into the tree once. Set-operation lookups borrow packed input items.
QUERY intersects dimension-zero bounds, seeks by rank, and compares residual
filters using ordered bits. Score replies allocate their SDS once.

Random sampling: `--suite random --n 100000 --dims 4 --repeats 7` and
`--n 1000000 --dims 4 --repeats 5`, with the same server configuration.
The percentages below are distinct sample sizes relative to cardinality.

| Cardinality / sample | Baseline server ms | Candidate server ms | Server speedup | Client wall speedup |
|---|---:|---:|---:|---:|
| 100K / 1% | 0.3595 | 0.2275 | 1.58× | 1.16× |
| 100K / 10% | 3.6195 | 2.4940 | 1.45× | 1.12× |
| 100K / 25% | 8.8475 | 3.0570 | 2.89× | 1.30× |
| 100K / 90% | 25.7370 | 7.2575 | 3.55× | 1.23× |
| 100K / 100% | 11.2065 | 7.5205 | 1.49× | 1.04× |
| 1M / 25% | 159.7620 | 63.1235 | 2.53× | 1.36× |
| 1M / 50% | 355.8225 | 113.4525 | 3.14× | 1.41× |
| 1M / 90% | 700.6555 | 180.3975 | 3.88× | 1.44× |
| 1M / 100% | 318.4015 | 201.2140 | 1.58× | 1.09× |

Dense distinct samples walk the tree once into a borrowed-pointer array, then
use partial Fisher-Yates swaps. The array uses `sizeof(pointer) * N` temporary
bytes. Sparse samples only store positions needed by future swaps. Single-input
INTERCARD reads the existing cardinality and clamps LIMIT: 100K inputs went
from 2.10 ms to below 1 µs; 1M inputs went from 39.52 ms to below 1 µs.
Sub-microsecond command timers are quantized; no precise speedup is claimed.

POP: `--suite pop --n 5000 --dims 16 --repeats 9` copies the source before
each repeat, outside the timer. POPMAX count 1 improved from 1.536 to 1.182 µs
(1.30× server, essentially unchanged client wall time); count 100 improved
from 102.26 to 75.26 µs (1.36× server, 1.11× client wall time).
Duplicate sampling has no claimed speedup: a focused alternating comparison
on 30K / 16 dimensions measured baseline medians 8.96–9.49 µs and candidate
medians 9.45–9.71 µs, with effectively equal client wall time. It did not
reproduce the larger apparent slowdown in the short mixed-workload run.

Validation: full integration suite 5906 passed, unit tests 870 passed, final
ZVSET/ZSET regression 433 passed, ASan/UBSan ZVSET regression 69 passed.
The full suite used an isolated checkout and a frozen binary; the final
sparse-map store removal was subsequently covered by the targeted and
sanitizer runs. Lua was built with ASan only because its unchanged code
triggers UBSan alignment checks; the changed engine code used both sanitizers.
Formatting used clang-format-21 because clang-format-18 was unavailable.

Later dimensions still need a candidate scan; large replies remain limited by
transport and client parsing. These local closed-loop measurements do not
establish production tail latency. Small timings and three-repeat Tcl throughput
results vary substantially; the benchmark retains minima and maxima.
