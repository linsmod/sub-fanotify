#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 linsmod <linsmod@qq.com>
# e2e（issue #5）：DELETE 事件的路径不得带 " (deleted)" 后缀。
#
# 背景：sfa-server 曾用 find_fid_info(meta, 0) 宽松匹配任意 fid 记录，
# 正确性隐式依赖「DFID_NAME 排在 FID 之前」的内核记录顺序；顺序翻转时
# 每条 DELETE 的路径都会带 " (deleted)"，不报错不崩、日志齐整，只是内容错。
# 本脚本把那次实测变成可重复执行的一次性检查，使顺序相关回归在改动后
# 能被立刻发现。
#
# 需要 root（fanotify）。在支持 fanotify 的 Linux 上运行（WSL2 验证过）。
set -u
cd "$(dirname "$0")/.."

TMP=$(mktemp -d /tmp/sfa-e2e.XXXXXX) || exit 1

# 只 wait 自己起过的 PID：裸 `wait` 会等所有子进程，客户端是长驻进程时会挂住
PIDS=""
cleanup() {
    kill $PIDS 2>/dev/null
    wait $PIDS 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

MNT=$TMP/mnt
SOCK=$TMP/sfa.sock
mkdir -p "$MNT"

./sfa-server "$MNT" "$SOCK" 2>"$TMP/server.log" & SRV=$!
PIDS="$PIDS $SRV"
for i in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.1; done
[ -S "$SOCK" ] || { echo "FAIL: server 未能启动"; exit 1; }

./sfa_client "$SOCK" >"$TMP/C.out" 2>/dev/null & C=$!
PIDS="$PIDS $C"
sleep 0.5

# 先建后删：与 issue #5 的实测同形状（40 个空文件，一次性 rm）
mkdir -p "$MNT/probe"
for i in $(seq 1 40); do : > "$MNT/probe/f$i"; done
sleep 1
rm -f "$MNT/probe"/f*
sleep 2

DELETES=$(grep -c "DELETE" "$TMP/C.out" 2>/dev/null || true)
BAD=$(grep -c "(deleted)" "$TMP/C.out" 2>/dev/null || true)
SAMPLE=$(grep "DELETE" "$TMP/C.out" 2>/dev/null | head -1)

echo "DELETE 事件：$DELETES 条，其中带 \" (deleted)\" 后缀：$BAD 条"
echo "样例：$SAMPLE"

FAIL=0
[ "$DELETES" -ge 1 ] || { echo "FAIL: 没有收到任何 DELETE 事件，检查无从谈起"; FAIL=1; }
[ "$BAD" -eq 0 ]     || { echo "FAIL: DELETE 路径带 (deleted) 后缀 —— find_fid_info 的记录顺序前提被打破"; FAIL=1; }
[ $FAIL -eq 0 ] && echo "PASS: DELETE 路径无 (deleted) 后缀"
exit $FAIL
