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

# 能力探测：make probe P=/data
probe: sfa-server
	./sfa-server --probe $(or $(P),/)

.PHONY: all clean check probe
