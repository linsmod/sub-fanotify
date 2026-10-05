# sfa

A privileged fanotify proxy that turns Linux filesystem events into a local socket
stream, so an unprivileged indexer can stay current without polling.

Written in C11, no runtime dependencies beyond libc. Target: Linux (ext4).

---

## What this is for

`esidx` keeps a search index in line with the filesystem by walking it. A walk
that skips unchanged directories is 0.1 ms idle, but it only sees *names*: a file
edited in place changes nothing its parent can see. That gap is what SFA closes —
it reports every event as it happens, as an absolute path.

The split is deliberate. The watcher needs `CAP_SYS_ADMIN`; the indexer must not
have it. So one small privileged process (`sfa-server`) holds the fanotify group
and rebroadcasts events over an `AF_UNIX`/`SOCK_SEQPACKET` socket, and any number
of unprivileged clients subscribe with a bitmask. This repository is the whole of
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

## Build

```sh
make            # sfa-server and sfa_client
make check      # -fsyntax-only over every translation unit, no link, no artefacts
make probe P=/  # capability report for a mount point
make clean
```

Requires GCC or Clang with C11 and `make`.

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

# watch it, unprivileged
./sfa_client                              # default socket
./sfa_client /tmp/sfa.sock
#   CREATE|CLOSE_WRITE|ATTRIB       pid=31337      path=/data/a.txt
#   MOVED                          pid=31337      path=/data/b.txt  old=/data/a.txt
```

## Protocol

Fixed-size structs over `SOCK_SEQPACKET`, so one `recv` is one message and a
truncated message is a protocol error rather than a half-parsed event. `sfa.h`
is the whole definition.

| Direction | Message | |
|---|---|---|
| server → client | `struct sfa_welcome` | protocol version + the mount being watched, sent on connect |
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

The SDK is four calls — `sfa_connect`, `sfa_subscribe`, `sfa_recv`, `sfa_close`
— plus `sfa_event_path` / `sfa_event_path2` / `sfa_event_is_dir` for the fields.
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

## Environment

This directory is its own git repository, nested inside the `esidx` repository
one level up — it shares no history with it and is versioned independently.
The watcher is a separate concern from the indexer it feeds; that separation is
the reason it is not a translation unit inside `esidx`.