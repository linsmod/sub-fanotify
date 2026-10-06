# sfa

A privileged fanotify proxy that turns Linux filesystem events into a local socket
stream, so an unprivileged indexer can stay current without polling.

Written in C11, no runtime dependencies beyond libc. Target: Linux (ext4).

---

## What this is for

The motivating case is a search index kept in line with the filesystem by
walking it. A walk that prunes unchanged directories is cheap — a fraction of a
millisecond when nothing has changed — but it only ever sees *names*: a file
edited in place changes nothing its parent can see. That gap is what this closes,
reporting every event as it happens, as an absolute path.

SFA is developed for that case but is not tied to one indexer. What it offers is
the part that is hard to get right — a privileged capture process, and a wire
protocol between it and whoever consumes the events — so that anything keeping
derived state in step with the filesystem can use it.

The split is deliberate. The watcher needs `CAP_SYS_ADMIN`; the indexer must not
have it. So one small privileged process (`sfa-server`) holds the fanotify group
and rebroadcasts events over an `AF_UNIX`/`SOCK_SEQPACKET` socket, and any number
of unprivileged clients subscribe with a bitmask. The socket is `0600 root:root`
unless the server is started with `--group <name>`, which makes it `0660` and
owned by that group — see [Usage](#usage). This repository is the whole of
that: the proxy, the protocol, a ~100-line client SDK and a sample client.

## Status

Builds and runs. `--probe` reports what the local kernel and privileges actually
support, and the server refuses to start if the answer is no.

| Capability | Requirement | Without it |
|---|---|---|
| `FAN_REPORT_DFID_NAME` | kernel 4.19+ | server refuses to start |
| `open_by_handle_at` | `CAP_DAC_READ_SEARCH`, filesystem exportfs | warning; events without a path are dropped |
| `FAN_UNLIMITED_QUEUE` / `FAN_UNLIMITED_MARKS` | kernel 5.13+ | queue overflows under load |
| `FAN_RENAME` + `FAN_REPORT_TARGET_FID` | kernel 5.17+ | falls back to pairing `MOVED_FROM`/`MOVED_TO` |
| `FAN_ONDIR` | kernel 5.1+ | `mkdir`/`rmdir`/directory rename are filtered out by the kernel |

`FAN_MARK_FILESYSTEM` is a **fallback**, not an equivalent substitute for
`FAN_MARK_MOUNT`: it covers the whole filesystem rather than the mount point, and
it can engage backends that do not support exportfs. `--probe` reports both
(`MOUNT n/7, FILESYSTEM m/7`) so the coverage difference is visible before you
rely on it. Use `--prefix` to narrow what reaches clients.

Even with `open_by_handle_at` fully available, events whose parent directory is
deleted before the server reads them still cannot be resolved to a path — this
is a timing window in the fanotify API itself, not a missing capability. Such
events are not silently dropped: the server emits a `SFA_EV_UNRESOLVED` signal
event (empty path, subscribable like any other bit) once per read batch, plus a
loss count with errno breakdown on stderr. Clients subscribed to that bit
should treat it the same as `SFA_EV_OVERFLOW`: resync with a full pass.

## Build

```sh
make            # sfa-server and sfa_client
make check      # -fsyntax-only over every translation unit, no link, no artefacts
make selftest   # built-in self-check (pure logic, needs no privileges)
make e2e        # end-to-end scripts (needs root + fanotify)
make probe P=/  # capability report for a mount point
make install    # headers, libsfa.a and the two executables
make dist       # source package + prebuilt package, version = git describe (+ -dirty)
make clean
```

Requires GCC or Clang with C11 and `make`.

`make install` follows the usual conventions, so a downstream can put the SDK
wherever it wants:

```sh
make install                                # → /usr/local/{include,lib,bin}
make install PREFIX=$HOME/.local            # own prefix
make install PREFIX=/usr DESTDIR=/tmp/stage # staged root for packaging
make install-bin                            # only the two executables → $(PREFIX)/bin
make uninstall
```

It installs the contract (`sfa.h`), the probe header (`sfa_probe.h`, server-side
but handy for anyone wrapping fanotify), the client SDK (`libsfa.a`) and the
`sfa-server`/`sfa_client` executables — `install-bin` is the same minus the
development files, for machines that only run the proxy.

## Releases

`make dist` writes two artifacts into `dist/`, each with a `.sha256` next to it:

- `sfa-<version>.tar.gz` — the **source package**: all sources, the `Makefile`,
  this README and `issues/` (the known-limitations document). Rebuild everything
  from it with `make`.
- `sfa-<version>-bin.tar.gz` — the **prebuilt package**, structured as an SDK:
  `include/` (`sfa.h`, `sfa_probe.h`), `lib/` (`libsfa.a`) and `bin/` (the
  `sfa-server` and `sfa_client` executables). Its bytes depend on the build
  environment (compiler, libc, kernel headers).

The version string is `git describe` — `v1.2.0-3-gdeadbee`, or the bare commit
hash before the first tag — so a package always names the tree it came from.

**Any uncommitted change to a tracked *or* untracked file appends `-dirty`.** Such
a package is for your own testing; do not hand it out. Untracked files count
because they land in the tarball just as tracked ones do.

```sh
make dist
sha256sum -c dist/sfa-<version>.tar.gz.sha256
tar xzf dist/sfa-<version>.tar.gz
cat dist/sfa-<version>/VERSION       # same string, for the record
```

Owner and mtime are pinned to the commit's committer date, so the same commit
reproduces the same bytes — a guarantee that only the source package can make,
since the binaries in the `-bin` package vary with the toolchain. Only committed
files are packed: `issues/` is collected through `git ls-files`, so untracked
issues are left out by construction. `issues/` is the known-limitations document
for a release and travels with the package once it is committed.

## Usage

```sh
# what does this machine support?
./sfa-server --probe /
#   kernel 5.15.0-generic
#   CAP_SYS_ADMIN yes, CAP_DAC_READ_SEARCH yes
#   FAN_REPORT_DFID_NAME ok, FAN_UNLIMITED_QUEUE ok, FAN_RENAME ok
#   usable

# run the proxy (needs privileges)
sudo ./sfa-server /                       # default socket /run/sfa.sock
./sfa-server /data /tmp/sfa.sock          # explicit mount and socket

# let a specific group of unprivileged users in. The socket stays 0660 root:GROUP —
# events leak every filename on the filesystem, so world-readable is not an option,
# and the client user must be a member of GROUP.
sudo ./sfa-server / --group esidx
#   sfa-server: mount=/ socket=/run/sfa.sock mode=0660 owner=root:esidx

# only forward events under these prefixes (repeatable, matched by path component)
sudo ./sfa-server / --prefix /home --prefix /srv

# watch it, unprivileged
./sfa_client                              # default socket
./sfa_client /tmp/sfa.sock
#   CREATE|CLOSE_WRITE|ATTRIB       pid=31337      path=/data/a.txt
#   MOVED                          pid=31337      path=/data/b.txt  old=/data/a.txt
```

Without `--group` the socket is `0600 root:root`, which means only root can
connect. That is the safe default, and it is also why a non-root client needs
`--group` on the server side: group membership is a deployment concern, not
something the proxy can grant.

## Embedding the server

The proxy is also available as a library, for callers that are **already a
privileged daemon** and would rather have the watcher as a component instead of
a child process:

```c
#include "sfa_server.h"

struct sfa_srv_opts opts = { .mount = "/data" };
struct sfa_srv *srv;
if (sfa_srv_open(&srv, &opts) < 0) return 1;   /* 原因经日志回调/ sfa_srv_error */
sfa_srv_run(srv);                              /* blocks until sfa_srv_stop() */
sfa_srv_close(srv);
```

`sfa_srv_stop()` is async-signal-safe (it sets a flag and writes to an internal
self-pipe that wakes `poll`), so it can be called from a signal handler or from
another thread. To drive it from your own event loop, poll `sfa_srv_fd()` and
then call `sfa_srv_poll(srv, 0)`. Several instances can coexist in one process.

Everything lives in the one archive: link `libsfa.a`. Static archives are pulled
in per object file, so a client that only calls `sfa_connect` does **not** drag
the server objects (or their fanotify dependency) into its binary — the linker
decides that, not the packaging. (Under a shared library this would stop being
true, which is when splitting or symbol visibility would start to matter.)

**Two constraints to weigh before choosing this shape:**

1. **Privilege boundary.** fanotify needs `CAP_SYS_ADMIN`, so the embedding
   process is privileged by definition. If the caller is an unprivileged
   indexer, embedding the watcher would give the indexer root — which is exactly
   the problem this project exists to avoid. Keep the child-process split there.
2. **Merged failure domain.** A crash in the watcher now takes the caller down
   with it; with the child-process model, only the proxy dies.

## Protocol

Fixed-size structs over `SOCK_SEQPACKET`, so one `recv` is one message and a
truncated message is a protocol error rather than a half-parsed event. `sfa.h`
is the whole definition.

| Direction | Message | |
|---|---|---|
| server → client | `struct sfa_welcome` | protocol version, the mount being watched and the server's working-mode flags (`SFA_WF_*`: mark scope, rename semantics, `ONDIR`, prefix filtering, path lookup), sent on connect |
| client → server | `struct sfa_subscribe_req` | event bitmask; `0` unsubscribes |
| server → client | `struct sfa_event` | `type`/`mask`/`pid`/`flags`/`timestamp` + up to 4 KB of path |

Three decisions are worth knowing before reading the code:

- **One kernel event is one `sfa_event`, never split.** The kernel merges changes
  on one object into one event — `touch` yields `CREATE|CLOSE_WRITE|ATTRIB` — and
  splitting would discard the fact that they were a single operation, while
  inflating a 4 KB message into several. `type` is the lowest set bit of `mask`,
  so a client that only wants the primary semantic can `switch (ev.type)` and one
  that wants all of it can test `ev.mask & SFA_EV_X`.
- **rename arrives as one event on 5.17+.** `SFA_EV_MOVED` carries the new path
  in `path` and the old one in `path2_off`; the server drops
  `MOVED_FROM`/`MOVED_TO` in that mode so one rename is not reported twice. On
  older kernels the two halves must be paired by the client.
- **Subscription filters on the full mask, not on the primary event.** Because
  the kernel merges bits, filtering by `type` alone would silently starve a
  client that subscribed to any other bit of the same event.

The SDK is four calls — `sfa_connect` (or `sfa_connect2`, which also hands back
the `sfa_welcome`: protocol version, the watched mount and the `SFA_WF_*` working
mode flags), `sfa_subscribe`, `sfa_recv`, `sfa_close` — plus `sfa_event_path` /
`sfa_event_path2` / `sfa_event_is_dir` for the fields and `sfa_work_flags_str`
for printing the mode.
Into an index, an event is an idempotent upsert by path; that is the whole
integration.

## Layout

```
sfa.h        protocol: event types, message structs, SDK declarations
sfa_probe.h  fanotify capability struct + probe API (server only)
sfa_probe.c  the probe: what this kernel/privilege set supports, and the
             FAN_* constants older glibc headers do not define
sfa-server.c the proxy: fanotify group, path resolution, broadcast, subscribe
libsfa.c     client SDK
sfa_client.c sample client: connect, subscribe, print
```

`SFA_SOCKET_PATH` is `/run/sfa.sock`, overridable by the server's second
argument and the client's first.

## Independence

This is a standalone project: its own repository, its own history, its own release
versions. It is not a component of the indexer it serves — the split described
above is the reason that separation exists.

Being checked out inside a larger working tree is a placement decision, not a
dependency. Nothing here compiles against, links against, or is imported by
whatever hosts it; `make dist` produces a self-contained tarball that builds on
its own, which is the practical test of that claim.

The consequence worth stating plainly: `sfa.h` is the contract, not any consumer's
source tree. Breaking changes go through a protocol version bump, and
`sfa_connect()` refuses a version it does not know rather than behaving
inexplicably.


## Owner/Maintanier should knows
Check AGENTS.md for requirements & details