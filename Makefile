CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?=

# 安装位置（GNU 惯例）：PREFIX 是最终前缀，DESTDIR 只用于打包暂存。
# 下游可以整条命令行覆盖，例如：
#   make install PREFIX=$HOME/.local          # 装到自己的 prefix
#   make install PREFIX=/usr DESTDIR=/tmp/stage   # 打包暂存到 /tmp/stage/usr/...
PREFIX     ?= /usr/local
DESTDIR    ?=
INCLUDEDIR ?= $(PREFIX)/include
LIBDIR     ?= $(PREFIX)/lib
BINDIR     ?= $(PREFIX)/bin

all: sfa-server sfa_client

# CLI 是薄包装：逻辑在 sfa_server.c（库）里（issue #10）
sfa-server: sfa-server.c sfa_server.c sfa_probe.c sfa.h sfa_probe.h sfa_server.h
	$(CC) $(CFLAGS) -o $@ sfa-server.c sfa_server.c sfa_probe.c $(LDFLAGS)

sfa_client: sfa_client.c libsfa.c sfa.h
	$(CC) $(CFLAGS) -o $@ sfa_client.c libsfa.c $(LDFLAGS)

# 单一归档：客户端 SDK + 服务端库（sfa_srv_*）+ 能力探测。
#
# 为什么一个库就够：静态归档是**按符号拉取目标文件**的 —— 只调 sfa_connect 的
# 消费方，链接器不会把 sfa_server.o / sfa_probe.o 拉进它的二进制，所以
# 「客户端库保持不需要特权」这件事由链接器保证，不必靠拆归档。
# （代价只在共享库形态下出现：.so 会把所有目标文件链进去，那时才需要拆库或用
#   符号可见性控制；本项目发的是 .a。）
libsfa.a: libsfa.c sfa_server.c sfa_probe.c sfa.h sfa_server.h sfa_probe.h
	$(CC) $(CFLAGS) -c libsfa.c     -o libsfa.o
	$(CC) $(CFLAGS) -c sfa_probe.c  -o sfa_probe.o
	$(CC) $(CFLAGS) -c sfa_server.c -o sfa_server.o
	ar rcs $@ libsfa.o sfa_probe.o sfa_server.o
	@rm -f libsfa.o sfa_probe.o sfa_server.o
# 注意 sfa-server.c（含 main）**不能**进归档：调用方一旦引用 sfa_srv_open，
# 同一个目标文件里的 main 会被一并拉进来，与调用方自己的 main 冲突。

clean:
	rm -f sfa-server sfa_client libsfa.a *.o

# 安装：头文件（sfa.h 契约 / sfa_probe.h / sfa_server.h）+ 库 + 可执行文件。
# install-bin 只装可执行文件（只要部署物、不做开发的场景）。
install: all libsfa.a
	@mkdir -p "$(DESTDIR)$(INCLUDEDIR)" "$(DESTDIR)$(LIBDIR)" "$(DESTDIR)$(BINDIR)"
	install -m 0644 sfa.h sfa_probe.h sfa_server.h "$(DESTDIR)$(INCLUDEDIR)/"
	install -m 0644 libsfa.a "$(DESTDIR)$(LIBDIR)/"
	install -m 0755 sfa-server sfa_client "$(DESTDIR)$(BINDIR)/"
	@echo "installed: $(DESTDIR)$(INCLUDEDIR)/{sfa.h,sfa_probe.h,sfa_server.h}"
	@echo "installed: $(DESTDIR)$(LIBDIR)/libsfa.a"
	@echo "installed: $(DESTDIR)$(BINDIR)/{sfa-server,sfa_client}"

install-bin: all
	@mkdir -p "$(DESTDIR)$(BINDIR)"
	install -m 0755 sfa-server sfa_client "$(DESTDIR)$(BINDIR)/"
	@echo "installed: $(DESTDIR)$(BINDIR)/{sfa-server,sfa_client}"

uninstall:
	rm -f "$(DESTDIR)$(INCLUDEDIR)/sfa.h" "$(DESTDIR)$(INCLUDEDIR)/sfa_probe.h" \
	      "$(DESTDIR)$(INCLUDEDIR)/sfa_server.h"
	rm -f "$(DESTDIR)$(LIBDIR)/libsfa.a"
	rm -f "$(DESTDIR)$(BINDIR)/sfa-server" "$(DESTDIR)$(BINDIR)/sfa_client"
	@echo "uninstalled from $(DESTDIR)$(PREFIX)"

# 语法/语义检查：只编译不链接，快速验证改动
check: sfa.h sfa_probe.h sfa_server.h
	$(CC) $(CFLAGS) -fsyntax-only sfa-server.c
	$(CC) $(CFLAGS) -fsyntax-only sfa_server.c
	$(CC) $(CFLAGS) -fsyntax-only sfa_probe.c
	$(CC) $(CFLAGS) -fsyntax-only libsfa.c
	$(CC) $(CFLAGS) -fsyntax-only sfa_client.c
	@echo "syntax OK"

# 自检：纯逻辑的可运行检查（目前是 --prefix 的路径分量比较）。
# 仓库没有测试框架，这类错了不会报错、又不会崩的逻辑必须有入口能跑。
selftest: sfa-server
	./sfa-server --selftest

# 端到端：起真实服务端 + 客户端，验证背压与丢失信号（scripts/test_*.sh）。
# 需要 root 与支持 fanotify 的 Linux（WSL2 验证过）；任一脚本失败即停。
# 每个脚本套一个硬超时：脚本自己挂住时要报错退出，不能把终端吊死。
E2E_TIMEOUT ?= 60
e2e: all
	@set -e; for t in scripts/test_*.sh; do \
	    echo "== $$t"; \
	    timeout $(E2E_TIMEOUT) ./"$$t" || { \
	        rc=$$?; echo "FAIL: $$t（退出码 $$rc$$( [ $$rc -eq 124 ] && echo '，超时 $(E2E_TIMEOUT)s' )）"; exit $$rc; }; \
	done
	@echo "e2e: all passed"

# 能力探测：make probe P=/data
probe: sfa-server
	./sfa-server --probe $(or $(P),/)

# ---- 发行包：可追溯到具体 commit，工作区不干净时版本串带 -dirty ----
#
# 版本串 = git describe（tag-N-gHASH；无 tag 时就是 HASH），工作区有任何未提交
# 改动时追加 "-dirty"。
# dirty 必须用 `git status --porcelain`判定，不能用 describe --dirty：后者只比对
# 已跟踪文件，看不见未跟踪文件（issues/ 就是），而未跟踪的改动同样会流进包里。
#
# 文件按显式清单取，不递归打包整个目录。dist/ 自身在 .gitignore 里——否则打包
# 产物会让下一次 status 永远为 dirty，版本串再也回不到干净态。

GIT       := git
REVISION  := $(shell $(GIT) rev-parse --short=12 HEAD 2>/dev/null)
DESCRIBE  := $(shell $(GIT) describe --tags --always 2>/dev/null)
DIRTY     := $(shell test -z "$$($(GIT) status --porcelain 2>/dev/null)" || echo -dirty)
VERSION   := $(if $(DESCRIBE),$(DESCRIBE),$(REVISION))$(DIRTY)
MTIME     := $(shell $(GIT) show -s --format=%ct HEAD 2>/dev/null || echo 0)

SRCFILES  := sfa.h sfa_probe.h sfa_server.h sfa_probe.c sfa_server.c sfa-server.c libsfa.c sfa_client.c
DOCFILES  := README.md
# issues/ 是两层的：规范在根，issue 在 open/ 与 closed/。只取 git 已跟踪的
# （git ls-files）——这直接兑现 README「only committed files are packed」的承诺，
# 也让 dist 的校验不再需要针对 issues/ 单独告警：未提交的 issue 本来就进不了包。
ISSUES    := $(shell $(GIT) ls-files 'issues/*.md' 'issues/*/*.md' 2>/dev/null)
DISTFILES := Makefile $(SRCFILES) $(DOCFILES)

DISTDIR   := dist
PKGNAME   := sfa-$(VERSION)
STAGE     := $(DISTDIR)/.stage-$(PKGNAME)
PKG       := $(DISTDIR)/$(PKGNAME).tar.gz

# 编译包：include/（头文件）+ lib/（静态 SDK）+ bin/（可执行文件）。
# 内容随编译环境（编译器版本、libc、内核头）变化，不承诺跨机字节一致；
# 源码包保持确定性（同 commit 同字节），可复现构建请用源码包。
BINNAME   := sfa-$(VERSION)-bin
BINSTAGE  := $(DISTDIR)/.stage-$(BINNAME)
BINPKG    := $(DISTDIR)/$(BINNAME).tar.gz

# 固定 owner 与 mtime（取 commit 的提交时间），源码包同一 commit 重复打包得到
# 同样的字节。需要 GNU tar >= 1.28（--sort）。
dist: all libsfa.a
	@$(GIT) rev-parse --git-dir >/dev/null 2>&1 || \
	    { echo "make dist 需要在 sfa 这个 git 仓库内运行"; exit 1; }
	@rm -rf "$(STAGE)" "$(BINSTAGE)"
	@mkdir -p "$(STAGE)/$(PKGNAME)/issues"
	@cp $(DISTFILES) "$(STAGE)/$(PKGNAME)/"
	@for f in $(ISSUES); do \
	    mkdir -p "$(STAGE)/$(PKGNAME)/$$(dirname "$$f")"; \
	    cp "$$f" "$(STAGE)/$(PKGNAME)/$$f"; \
	done
	@printf '%s\n' '$(VERSION)' > "$(STAGE)/$(PKGNAME)/VERSION"
	@tar --sort=name --owner=0 --group=0 --numeric-owner \
	     --mtime='@$(MTIME)' -C "$(STAGE)" -czf "$(PKG)" "$(PKGNAME)"
	@mkdir -p "$(BINSTAGE)/$(BINNAME)/include" "$(BINSTAGE)/$(BINNAME)/lib" \
	          "$(BINSTAGE)/$(BINNAME)/bin"
	@cp sfa.h sfa_probe.h sfa_server.h "$(BINSTAGE)/$(BINNAME)/include/"
	@cp libsfa.a "$(BINSTAGE)/$(BINNAME)/lib/"
	@cp sfa-server sfa_client "$(BINSTAGE)/$(BINNAME)/bin/"
	@printf '%s\n' '$(VERSION)' > "$(BINSTAGE)/$(BINNAME)/VERSION"
	@tar --sort=name --owner=0 --group=0 --numeric-owner \
	     --mtime='@$(MTIME)' -C "$(BINSTAGE)" -czf "$(BINPKG)" "$(BINNAME)"
	@rm -rf "$(STAGE)" "$(BINSTAGE)"
	@echo "---"
	@echo "package : $(PKG)"
	@echo "bin pkg : $(BINPKG)"
	@echo "version : $(VERSION)"
	@echo "commit  : $(REVISION)"
	@echo "dirty   : $(if $(DIRTY),yes,no)"
	@sha256sum "$(PKG)" "$(BINPKG)" | tee "$(PKG).sha256"

distclean: clean
	rm -rf $(DISTDIR)

.PHONY: all clean check selftest e2e probe install install-bin uninstall dist distclean
