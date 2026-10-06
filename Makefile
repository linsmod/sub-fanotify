CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?=

all: sfa-server sfa_client

sfa-server: sfa-server.c sfa_probe.c sfa.h sfa_probe.h
	$(CC) $(CFLAGS) -o $@ sfa-server.c sfa_probe.c $(LDFLAGS)

sfa_client: sfa_client.c libsfa.c sfa.h
	$(CC) $(CFLAGS) -o $@ sfa_client.c libsfa.c $(LDFLAGS)

# 客户端 SDK 静态库：编译包（make dist 的 lib/）与源码包之外的第三个产物
libsfa.a: libsfa.c sfa.h
	$(CC) $(CFLAGS) -c libsfa.c -o libsfa.o
	ar rcs $@ libsfa.o
	@rm -f libsfa.o

clean:
	rm -f sfa-server sfa_client libsfa.a libsfa.o

# 语法/语义检查：只编译不链接，快速验证改动
check: sfa.h sfa_probe.h
	$(CC) $(CFLAGS) -fsyntax-only sfa-server.c
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
e2e: all
	@set -e; for t in scripts/test_*.sh; do \
	    echo "== $$t"; ./"$$t"; \
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

SRCFILES  := sfa.h sfa_probe.h sfa_probe.c sfa-server.c libsfa.c sfa_client.c
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

# 编译包：include/ + lib/（头文件、静态 SDK、两个可执行文件）。
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
	@mkdir -p "$(BINSTAGE)/$(BINNAME)/include" "$(BINSTAGE)/$(BINNAME)/lib"
	@cp sfa.h sfa_probe.h "$(BINSTAGE)/$(BINNAME)/include/"
	@cp libsfa.a sfa-server sfa_client "$(BINSTAGE)/$(BINNAME)/lib/"
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

.PHONY: all clean check selftest probe dist distclean
