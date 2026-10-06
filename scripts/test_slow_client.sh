#!/usr/bin/env bash
# e2e（issue #1）：一个不读数据的客户端（SIGSTOP 造出）不能拖住其他订阅者。
#
# 修复前（阻塞 send）的实测基线：A 被 STOP 后，健康客户端 B 收到约 26 条就
# 停住，事件一直在产生也收不到；A 关掉后才一次性追上。
# 修复后：B 在 A 存在期间持续收到事件；服务端 stderr 出现 desync 记录；
# A 恢复后收到补发的丢失信号（UNRESOLVED）。
#
# 需要 root（fanotify）。在支持 fanotify 的 Linux 上运行（WSL2 验证过）。
set -u
cd "$(dirname "$0")/.."

TMP=$(mktemp -d /tmp/sfa-e2e.XXXXXX) || exit 1
trap 'kill $SRV $A $B 2>/dev/null; wait 2>/dev/null; rm -rf "$TMP"' EXIT

MNT=$TMP/mnt
SOCK=$TMP/sfa.sock
mkdir -p "$MNT"

./sfa-server "$MNT" "$SOCK" 2>"$TMP/server.log" & SRV=$!
for i in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.1; done
[ -S "$SOCK" ] || { echo "FAIL: server 未能启动"; exit 1; }

./sfa_client "$SOCK" >"$TMP/A.out" 2>/dev/null & A=$!
sleep 0.5
kill -STOP $A            # A 停止读取，socket 缓冲将满

./sfa_client "$SOCK" >"$TMP/B.out" 2>/dev/null & B=$!
sleep 0.5

count() { grep -c "path=" "$TMP/B.out" 2>/dev/null || true; }

mkdir -p "$MNT/bulk"
for i in $(seq 1 400); do echo x > "$MNT/bulk/b$i"; done
sleep 1
N1=$(count)

for i in $(seq 401 800); do echo x > "$MNT/bulk/b$i"; done
sleep 2
N2=$(count)

kill -CONT $A            # A 恢复读取
sleep 2

UNRES_A=$(grep -c UNRESOLVED "$TMP/A.out" 2>/dev/null || true)
DESYNC=$(grep -c desync "$TMP/server.log" 2>/dev/null || true)

echo "健康客户端 B：第一批后 $N1 条，第二批后 $N2 条"
echo "A 恢复后收到的 UNRESOLVED 信号：$UNRES_A"
echo "服务端 stderr desync 记录：$DESYNC 行"

FAIL=0
[ "$N1" -ge 100 ]    || { echo "FAIL: 健康客户端第一批收到的事件过少（$N1）"; FAIL=1; }
[ "$N2" -gt "$N1" ]  || { echo "FAIL: A 停止读取期间 B 收不到事件（阻塞 send 未修？）"; FAIL=1; }
[ "$DESYNC" -ge 1 ]  || { echo "FAIL: 服务端没有记录 desync"; FAIL=1; }
[ "$UNRES_A" -ge 1 ] || { echo "FAIL: A 恢复后没有收到丢失信号"; FAIL=1; }
[ $FAIL -eq 0 ] && echo "PASS: 慢客户端不再拖住其他订阅者"
exit $FAIL
