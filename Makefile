CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?=

all: sfa-server sfa_client

sfa-server: sfa-server.c sfa_probe.c sfa.h sfa_probe.h
	$(CC) $(CFLAGS) -o $@ sfa-server.c sfa_probe.c $(LDFLAGS)

sfa_client: sfa_client.c libsfa.c sfa.h
	$(CC) $(CFLAGS) -o $@ sfa_client.c libsfa.c $(LDFLAGS)

clean:
	rm -f sfa-server sfa_client

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
# issues/ 是两层的：规范在根，issue 在 open/ 与 closed/。wildcard 不递归，
# 只写 issues/*.md 会把全部 issue 漏掉，而 README 承诺它们随包发布。
ISSUES    := $(wildcard issues/*.md issues/*/*.md)
DISTFILES := Makefile $(SRCFILES) $(DOCFILES)

DISTDIR   := dist
PKGNAME   := sfa-$(VERSION)
STAGE     := $(DISTDIR)/.stage-$(PKGNAME)
PKG       := $(DISTDIR)/$(PKGNAME).tar.gz

# 固定 owner 与 mtime（取 commit 的提交时间），同一 commit 重复打包得到同样的
# 字节。需要 GNU tar >= 1.28（--sort）。
dist:
	@$(GIT) rev-parse --git-dir >/dev/null 2>&1 || \
	    { echo "make dist 需要在 sfa 这个 git 仓库内运行"; exit 1; }
	@rm -rf "$(STAGE)"
	@mkdir -p "$(STAGE)/$(PKGNAME)/issues"
	@cp $(DISTFILES) "$(STAGE)/$(PKGNAME)/"
	@for f in $(ISSUES); do \
	    mkdir -p "$(STAGE)/$(PKGNAME)/$$(dirname "$$f")"; \
	    cp "$$f" "$(STAGE)/$(PKGNAME)/$$f"; \
	done
	@printf '%s\n' '$(VERSION)' > "$(STAGE)/$(PKGNAME)/VERSION"
	@tar --sort=name --owner=0 --group=0 --numeric-owner \
	     --mtime='@$(MTIME)' -C "$(STAGE)" -czf "$(PKG)" "$(PKGNAME)"
	@rm -rf "$(STAGE)"
	@echo "---"
	@echo "package : $(PKG)"
	@echo "version : $(VERSION)"
	@echo "commit  : $(REVISION)"
	@echo "dirty   : $(if $(DIRTY),yes,no)"
	@if [ -n "$(ISSUES)" ] && $(GIT) status --porcelain -- $(ISSUES) | grep -q .; then \
	    echo "warn    : issues/ 尚未提交，包内容会随工作区变化"; \
	fi
	@sha256sum "$(PKG)" | tee "$(PKG).sha256"

distclean: clean
	rm -rf $(DISTDIR)

.PHONY: all clean check selftest probe dist distclean
