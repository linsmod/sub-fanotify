#!/usr/bin/env bash
# 临时：验证 #10 全部验收标准（勿提交）
set -u
REPO=/mnt/c/Users/linswin/AndroidStudioProjects/ShareToPC/esidx/sfa
cd /home/linsmod/sfa-e2e || exit 1
cp "$REPO"/{sfa.h,sfa_probe.h,sfa_server.h,sfa_probe.c,sfa_server.c,sfa-server.c,libsfa.c,sfa_client.c,Makefile} .
mkdir -p scripts
cp "$REPO"/scripts/*.sh "$REPO"/scripts/*.c scripts/
sed -i 's/\r$//' scripts/*.sh scripts/*.c
chmod +x scripts/*.sh

make clean >/dev/null
make check || exit 1
make >/dev/null || exit 1
make libsfa.a libsfa-server.a libsfa-all.a || exit 1
echo "--- 归档 ---"; ls -l libsfa.a libsfa-server.a libsfa-all.a

echo "--- 验收 5：libsfa.a 不含 main / fanotify 符号 ---"
if nm -g --defined-only libsfa.a | grep -qE ' (T|D) main$'; then
    echo "FAIL: libsfa.a 里有 main"; else echo "ok: 无 main"; fi
if nm -u libsfa.a | grep -q fanotify; then
    echo "FAIL: libsfa.a 依赖 fanotify"; else echo "ok: 无 fanotify 依赖"; fi
echo "libsfa.a 符号："; nm -g --defined-only libsfa.a | grep ' T ' | awk '{print "   "$3}'
echo "libsfa-server.a 符号："; nm -g --defined-only libsfa-server.a | grep ' T ' | awk '{print "   "$3}'
if nm -g --defined-only libsfa-server.a | grep -qE ' (T|D) main$'; then
    echo "FAIL: libsfa-server.a 里有 main（会与调用方冲突）"; else echo "ok: 服务端库无 main"; fi

make selftest

echo "--- 验收 6：install ---"
D=$(mktemp -d /tmp/sfa-inst.XXXXXX)
make install PREFIX=/opt/sfa DESTDIR="$D" >/dev/null || exit 1
find "$D" -type f -printf '%m %P\n' | sort -k2
make uninstall PREFIX=/opt/sfa DESTDIR="$D" >/dev/null
rm -rf "$D"

echo "--- 验收 1/2/3/7：四条 e2e（root）---"
for t in scripts/test_*.sh; do
    echo "== $t"
    "./$t" || echo "FAIL: $t"
done
