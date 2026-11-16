# FLAC 解码器畸形输入加固报告

日期：2026-11-17　基线：59fa3402（xiph/flac master）　改动：`src/libFLAC/stream_decoder.c`、`include/FLAC/stream_decoder.h`

## 1. 目标

截断帧、非法元数据块、超大块长等畸形文件：解码必须返回错误码和偏移，不崩溃、不越界；合法文件逐样本不变。

## 2. 审计结论（内存安全）

对解码路径（`read_metadata_*`、`read_frame_*`、`read_subframe_*`、`bitreader`）逐项审计，现有防线完备：

- 元数据块读取由 `FLAC__bitreader_set_limit()` 限制在声明长度内，越界读取失败即返回；
- APPLICATION 块有 `real_length < 4` 下溢检查；PICTURE/VORBIS_COMMENT 各子长度与剩余限额比对；
- 帧头 blocksize 上限 65535 检查（`read_frame_header_`），输出缓冲按帧实际块长 `allocate_output_` 重新分配；
- 帧 CRC-16、样本 bps 范围（`OUT_OF_BOUNDS`）、Rice 参数与分区阶数均有校验。

经验证，本快照在 ASan+UBSan 下对全部畸形样本**无任何内存越界**（见第 6 节统计）。

## 3. 发现的缺陷与根因

**缺陷：畸形文件静默失败——无错误码、无偏移。** 在 1144 个畸形样本中，235 个（20.5%）在修复前不产生任何错误回调：

| 类别 | 数量 | 现象 | 根因 |
|---|---|---|---|
| 超大元数据块长（如 length=0xFFFFFF 超过文件剩余） | 100 | 静默 `END_OF_STREAM` 或停滞 `READ_METADATA` | EOF/限额失败直接 `return false`，未通知客户端 |
| 截断（元数据内、帧头内、子帧内、CRC 内） | 134 | 静默 `END_OF_STREAM` | 同上；帧中途 EOF 只走 rewind 重扫，多数情况扫不到错误 |
| 非法 STREAMINFO 长度（0/1/33） | 含上 | 停滞 `READ_METADATA` 状态 | `length < used_bits/8` 直接返回 false |
| 严重截断（< 4 字节，fLaC 魔数不全） | 含上 | 静默 `END_OF_STREAM` | `find_metadata_` EOF 直接返回 false |

客户端只能看到 `process_until_end_of_stream()` 返回 false + 状态码，无法区分"正常流结束"与"文件被截断"，也拿不到出错偏移。

## 4. 修复内容（src/libFLAC/stream_decoder.c）

仅为**畸形路径补充错误上报**，不改动任何解码运算，合法文件行为逐比特不变：

1. **`read_metadata_` 包装器**（原函数更名 `read_metadata_inner_`）：
   - 失败且状态停滞 `READ_METADATA`（块内容与声明长度不一致，如 STREAMINFO 长度 0）→ 上报 `FLAC__STREAM_DECODER_ERROR_STATUS_BAD_METADATA`；
   - 失败且状态为 `END_OF_STREAM`/`END_OF_LINK`（截断或超大块长越过 EOF）→ 上报 `BAD_METADATA`。**前提**：`FLAC__stream_decoder_get_decode_position()` 可用（有 tell 回调的真实流）。合法流的元数据段必然以 is_last 块结束并跟随帧，故此处的 EOF 必为畸形；而 `FLAC__metadata_object_set_raw()` 解析单个裸块时（无 tell 回调）恰好以边界 EOF 结束，属正常，不误报（此兼容性由 metaflac 测试套件验证）。
2. **`find_metadata_` 包装器**（原函数更名 `find_metadata_inner_`）：连 fLaC 魔数都读不全的严重截断 → 上报 `FLAC__STREAM_DECODER_ERROR_STATUS_LOST_SYNC`（已上报过则去重）。
3. **`read_frame_` 四处 EOF 点**：帧头读取、子帧读取、零填充读取、CRC-16 尾部读取时遇 EOF（帧被截断）→ 上报 `LOST_SYNC`。帧边界上的 EOF（正常流结束，发生在 `frame_sync_`）不上报。
4. **API 文档**（`include/FLAC/stream_decoder.h`）：`BAD_METADATA` 补充说明覆盖"元数据段中途遇到流结束（截断/块长越过文件尾）"。

**偏移获取**：错误回调内调用 `FLAC__stream_decoder_get_decode_position()` 即得出错字节偏移（`fuzz/decode_harness.c` 演示）。

## 5. 修复前后对比（1144 个畸形样本）

| 指标 | 修复前 | 修复后 |
|---|---|---|
| 静默失败（无错误码/偏移） | 235 | **0** |
| 错误码+偏移上报 | 433 | **668** |
| 合法/可容错解码（填充、注释、未知块类型等） | 476 | 476（不变） |
| 崩溃/越界（ASan） | 0 | 0 |

典型样本（`fuzz/samples/`，清单见 `manifest.csv`）：

```
d.trunc.000002.flac        -> LOST_SYNC   @ 2      （2 字节残桩）
d.trunc.000042.flac        -> BAD_METADATA @ 42    （STREAMINFO 后截断）
a.trunc.024305.flac        -> LOST_SYNC   @ 24305  （帧中途截断）
a.trunc.048610.flac        -> LOST_SYNC   @ 48609  （CRC 差 1 字节）
a.metahdr.len16777215@4    -> BAD_METADATA @ 48608 （块长 0xFFFFFF 越过 EOF）
a.metahdr.si_len0@4        -> BAD_METADATA @ 42    （STREAMINFO 长度 0）
a.frame.ch15@8311          -> BAD_HEADER  @ 8315   （非法声道分配）
a.frame.bs0@8311           -> UNPARSEABLE_STREAM @ 8317 （保留块长码）
```

按规范应当容错的情形保持静默：未知/保留元数据块类型（跳过）、STREAMINFO 中非法的 min/max blocksize/framesize 提示值（仅为提示，不参与缓冲分配）。

## 6. 零崩溃统计

- **畸形语料**：1144 个（截断 160、非法元数据头 280、STREAMINFO 非法组合 28、帧头腐坏 156、随机位翻转 480、块删除/复制等），ASan+UBSan 下 **0 崩溃、0 越界、0 超时**（库级 harness + `flac` 命令行工具各一遍）。
- **随机变异 fuzz**：150000 次迭代（位翻转/截断/块头腐坏/块增删/随机 seek 交错解码），ASan+UBSan 下 **0 崩溃、0 挂起**。
- **合法文件**：4 个种子（单/双声道、8k/44.1k/48k、16/24bit、正弦/噪声/斜坡）修复前后解码输出逐字节一致（raw MD5 比对），`flac -t` 全部通过。
- **回归**：官方测试套件 `ctest` 10/10 通过（libFLAC、libFLAC++、flac、metaflac、streams、seeking、compression、grabbag、replaygain、generate_streams）。
- **已知良性项**：`fixed.c`/`lpc.c` 的 `restore_signal` 在畸形残差下存在有符号整数溢出（UBSan 77/1144 报告），属音频重建路径回绕，上游明确认定为不可避免且无安全影响，oss-fuzz 构建以 `FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION` 屏蔽；不影响内存安全，生产构建（无 UBSan）行为不变。

## 7. 复现

```sh
# 构建（ASan+UBSan，与 oss-fuzz 同级）
cmake -B build-fuzz -DCMAKE_C_FLAGS="-fsanitize=address,undefined -g -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION" \
  -DBUILD_SHARED_LIBS=OFF -DWITH_OGG=OFF -DBUILD_CXXLIBS=OFF -DBUILD_PROGRAMS=OFF \
  -DBUILD_TESTING=OFF -DBUILD_DOCS=OFF -DINSTALL_MANPAGES=OFF
cmake --build build-fuzz -j
# 生成畸形样本并跑零崩溃统计
python3 fuzz/gen_malformed.py
./fuzz/run_all.sh            # 期望: TOTAL 1144  CRASHES 0
# 单样本错误码+偏移
gcc -fsanitize=address,undefined -g -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION \
  -Iinclude -Ibuild-fuzz/include fuzz/decode_harness.c -o fuzz/decode_harness \
  build-fuzz/src/libFLAC/libFLAC.a -lm
./fuzz/decode_harness fuzz/samples/a.trunc.024305.flac
# 长时 fuzz
./fuzz/fuzzer 150000
```
