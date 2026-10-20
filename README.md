# SisOp2: a Dropbox-like file sync service

A small Dropbox clone in C++17 for Linux, originally written for an Operating Systems II (distributed systems) course.

Each user gets a `sync_dir` folder, subfolders included. Whatever is created, changed or deleted there is sent to a server and copied to the user's other session. All traffic is encrypted with TLS, and users log in with a password. The server side is **replicated**: backup servers hold copies of every file and account. If the primary server dies, a backup is elected to take over, and clients move to it on their own.

> [!NOTE]
> This is a rewrite (October 2026) of the original 2023 assignment, which had serious bugs. They are listed in **[docs/AUDIT.md](docs/AUDIT.md)**, with how each one was resolved. The original code is still in the git history (branch `final`).

- [Quick start](#quick-start)
- [Using it](#using-it)
- [How it works](#how-it-works)
- [Testing](#testing)
- [Limitations](#limitations)

---

## Quick start

You need Linux (or WSL), g++ 9 or newer, and OpenSSL with its headers (`sudo apt install libssl-dev` on Debian/Ubuntu).

```bash
make
```

```bash
make certs
```

The first command builds `build/server` and `build/client`. The second creates a private certificate authority and a server certificate in `certs/` (see [Certificates](#certificates)).

Start a server in one terminal:

```bash
./build/server
```

And a client in another, logging in as `alice`:

```bash
./build/client alice 127.0.0.1 4000
```

The client asks for a password; the first login with a new username creates the account with that password. Anything you put in `./sync_dir`, including in subfolders, now reaches the server. A second `alice` session, started from another directory or another machine, gets it automatically.

---

## Using it

### Client

```bash
./build/client <username> <server_host> <server_port> [sync_dir] [--ca FILE]
```

- **Username:** letters, digits, `_` and `-`, up to 32 characters.
- **Password:** 6 to 256 characters. A new username is created on its first login, with the password given then. The client reads it from the `SISOP_PASSWORD` environment variable if set; otherwise it asks without echoing, or reads the first line of input when input isn't a terminal (handy for scripts).
- **Server certificate:** `--ca` is the CA certificate used to verify the server (default `certs/ca.pem`). Clients need only this file.
- **Sessions:** each user can have **two** sessions at a time.
- **Sync directory:** `sync_dir` defaults to `./sync_dir` and is created if it doesn't exist. Regular files in it and in its subfolders are synced. Symlinks are skipped, and empty folders aren't synced.
- **State file:** the client keeps its sync state in `sync_dir/.sisop_state`. Files and folders whose names start with `.sisop` are never synced.

When the client starts, it **reconciles** `sync_dir` with the server instead of overwriting either side. Changes made while the client wasn't running are applied in the right direction:
- files created or edited offline are uploaded;
- files deleted offline are deleted on the server;
- files changed elsewhere meanwhile are downloaded.

After that, every change is synced as it happens.

Commands, typed into the client's terminal:

| Command | What it does |
|---|---|
| `upload <path>` | Sends any file to the server. It then appears at the top of every session's `sync_dir`, including this one's. |
| `download <path>` | Copies a file from the server (a path as shown by `list_server`, e.g. `docs/notes.txt`) into the current directory. |
| `delete <path>` | Deletes a file on the server and in every session's `sync_dir`. |
| `list_server` | Lists your files on the server (name, size, modification time). |
| `list_client` | Lists the files in your `sync_dir`. |
| `sync` | Runs the reconciliation again; `get_sync_dir` does the same. |
| `help` | Shows the commands. |
| `exit` | Logs out and quits. Ctrl+D (end of input) does the same; Ctrl+C just quits, and the server notices. |

**Conflicts:** if a file was modified on two sides before they could sync (say, edited on a laptop that was offline), neither edit is lost. The server's version keeps the name, and the local one is saved next to it as `notes (conflicted copy 2026-10-20 14.03.07).txt`, which then syncs like any other file.

### Server

```bash
./build/server [--port PORT] [--host HOST] [--data DIR] [--certs DIR] [--join HOST:PORT] [--expect HOST:PORT,...]
```

| Option | Meaning |
|---|---|
| `--port` | Port to listen on. Default: 4000. |
| `--host` | The address other servers and clients should use to reach this server. Default: `127.0.0.1`, which is enough on one machine; on several machines, set it to this machine's IP or hostname. It must be listed in the server certificate. |
| `--data` | Where files are stored, as `DIR/users/<username>/<path>`, with accounts in `DIR/accounts/`. Default: `server_data_<port>`. |
| `--certs` | Directory holding `ca.pem`, `server.pem` and `server.key`. Default: `certs`. |
| `--join` | Join an existing cluster as a backup, by giving the address of any of its servers. Without it, the server starts a new cluster and is its primary. |
| `--expect` | The servers that should be in the cluster, comma-separated (this server may be included). A warning is raised as soon as one is missing; see [Expected servers](#expected-servers). |

### Certificates

All connections use TLS. The certificates come from a private certificate authority that belongs to your cluster:

```bash
tools/make-certs.sh certs 10.0.0.1 10.0.0.2 server3.example.org
```

This creates `certs/ca.pem` and `certs/ca.key` (the authority), plus `certs/server.pem` and `certs/server.key`. The server certificate is valid for `localhost`, `127.0.0.1` and every host you list; `make certs` lists none. Run the script again to add hosts: it keeps the existing authority and issues a new server certificate.

- **Servers** need `ca.pem`, `server.pem` and `server.key`. All servers can share the same files.
- **Clients** need only `ca.pem`.
- **Keep `ca.key` private.** Anyone holding it can issue certificates the cluster will accept as servers.

Set `SISOP_LOG=debug|info|warn|error` to change how much the server (default `info`) or client (default `warn`) logs to stderr.

### A replicated cluster

Three servers on one machine:

```bash
./build/server --port 4000
```

```bash
./build/server --port 4001 --join 127.0.0.1:4000
```

```bash
./build/server --port 4002 --join 127.0.0.1:4000
```

Clients can connect to any of them; backups redirect them to the primary. Every file is stored on all three servers.

If you stop the primary (port 4000), the backup that joined first (4001) takes over within a few seconds. Connected clients reconnect to it on their own and keep syncing. A server can join later through any member, and it receives a copy of every file when it joins.

On several machines, give each server its own `--host`, and make sure every host is in the certificate (see [Certificates](#certificates)):

```bash
./build/server --host 10.0.0.2 --join 10.0.0.1:4000
```

### Expected servers

The cluster survives crashes, but not network splits (see [Limitations](#limitations)). When everyone runs it locally, the useful thing is to know about a problem the moment it happens. Give every server the list of servers that should be up:

```bash
./build/server --port 4000 --expect 127.0.0.1:4000,127.0.0.1:4001,127.0.0.1:4002
```

```bash
./build/server --port 4001 --join 127.0.0.1:4000 --expect 127.0.0.1:4000,127.0.0.1:4001,127.0.0.1:4002
```

Every two seconds, each server checks that every expected server is a member of its cluster. For one that isn't, it asks that server directly which cluster it belongs to, which tells three cases apart:

| Warning | Meaning |
|---|---|
| `expected server … is not running (or can't be reached)` | It's down, or the network can't reach it. |
| `expected server … is running as a separate primary` | It's running a cluster of its own: either a network split, or it was started without `--join`. |
| `expected server … follows a different primary` | The cluster is split in two. |

How warnings are raised:
- **Immediately, in the server's log.** Ongoing problems are repeated every 30 seconds while they last.
- **On every client's terminal.** The primary pushes each warning to its clients, prefixed with `[cluster]`, and a client that connects while there's a problem is told right away.
- **When things recover,** every server that comes back is announced, followed by `all N expected servers are up`.

---

## How it works

### Layout

| Path | Contents |
|---|---|
| `common/protocol.*` | Message types, the frame format, `Writer`/`Reader` for payloads. |
| `common/connection.*` | `Connection`: one TLS connection with thread-safe sending, receive timeouts and request/reply helpers. |
| `common/net.*` | Connecting (name resolution, timeout, TLS handshake and certificate check), listening, accepting. |
| `common/tls.*` | OpenSSL setup: the certificates and verification rules. |
| `common/files.*` | Path validation, hashing, directory walking, and streaming a file over a connection. |
| `common/log.*` | A small thread-safe logger. |
| `server/server.*` | Accepts connections and serves client requests. |
| `server/accounts.*` | Usernames and salted password hashes. |
| `server/sessions.*` | Logged-in sessions (at most two per user). |
| `server/storage.*` | Files on disk, with atomic updates and a hash cache. |
| `server/cluster.*` | Replication, failure detection and leader election. |
| `server/health.*` | The `--expect` check for missing or separated servers. |
| `client/client.*` | Connection handling, reconciliation, the commands. |
| `client/sync_state.*` | The record of what was last in sync. |
| `client/watcher.*` | Recursive inotify on `sync_dir`. |
| `tools/make-certs.sh` | Creates the cluster's certificate authority and server certificate. |
| `tests/` | End-to-end tests. |

### Protocol

Everything travels as **framed messages** over TLS. Each message is an 8-byte header followed by its payload, all in network byte order:

```
uint16 type | uint16 reserved | uint32 payload length | payload ...
```

Payload fields are written by `Writer` and read by `Reader`: fixed-size integers, plus strings as a length followed by the bytes. A malformed payload or a frame announcing more than 1 MB closes the connection.

Files are streamed rather than packed into fixed-size packets:
1. a header message carrying the path (relative to the synced folder, e.g. `docs/notes.txt`), size and modification time;
2. any number of `FileData` messages of up to 64 KB each;
3. `FileEnd`.

If the sender can't read the file to the end, it sends `FileAbort` and the receiver discards what it got. Received files are written to a temporary file and **renamed** into place, so a reader never sees a half-written file.

All message types are listed, with their payloads, in `common/protocol.hpp`.

### Connections and sessions

Each client opens **two connections** to the primary:

- **Command connection.** It starts with `Login` (username and password), and the reply carries a random session token plus the list of servers in the cluster. From then on it carries request/reply exchanges, one at a time: `ListFiles`, `Upload`, `Download`, `Delete`, `Ping`, `Logout`.
- **Sync connection.** It starts with `SyncAttach(token)`. After that only the server talks on it, pushing the changes made by the user's other sessions (`PushFile`, `PushDelete`) and updates to the server list.

The server runs one thread per connection. The first message on a connection says what it is: a client login, a sync connection, or a message from another server. Sessions end on `Logout`, when either connection closes, or after 30 s of silence (clients ping every second).

### Security

- **Encryption:** every connection (client to server, and server to server) is TLS 1.2 or newer; in practice TLS 1.3 is negotiated.
- **Verifying the server:** clients check the server's certificate against the cluster's CA, including that the host they connected to is named in it. A machine pretending to be the server, or one in the middle of the connection, is refused.
- **Server-to-server messages:** servers present their certificate when they connect to each other (mutual TLS). Cluster messages (`Join`, `Election`, `Coordinator`, `ReplicaAttach`, `StatusRequest`) are only accepted from a peer with a valid certificate. A machine that isn't part of the cluster can't join it, call an election, or pose as a primary to receive copies of everyone's files.
- **Passwords:**
  - stored as PBKDF2-HMAC-SHA256 hashes, with a random 16-byte salt and 100,000 iterations, in `<data>/accounts/<user>`;
  - compared in constant time;
  - a wrong password is answered after a one-second delay, which slows down guessing.

  Accounts are replicated to backups like files, so passwords keep working after a failover.
- **Session tokens:** 128 random bits from OpenSSL's generator. A sync connection can only attach to a session by presenting its token.
- **Paths:** every file path coming from the network is validated before use. Paths are relative; `..`, `.`, empty components, control characters and reserved names are rejected, on the server and on the client alike. A client can't reach outside its user's folder, and a server can't make a client write outside `sync_dir`.

### Synchronization

The client compares **content hashes**, not filesystem events, to decide what to do. It keeps a record of every file as it was at the last successful sync: size, modification time and hash. That record is saved in `sync_dir/.sisop_state`.

- **Watching:** the watcher reports names, using `IN_CLOSE_WRITE`, `IN_MOVED_TO`, `IN_MOVED_FROM` and `IN_DELETE`, so files are reported once they're fully written. The worker thread then compares the file with the record:
  - it uploads the file if the content differs;
  - it deletes it on the server if the file is gone but the record has it;
  - it does nothing if they match.

  This makes duplicate events and the client's own downloads harmless. Downloads update the record **before** renaming the file into place, so the resulting event is recognized as nothing to do.
- **Subfolders:** inotify only watches one folder at a time, so the watcher adds a watch for every folder in the tree, including new ones as they appear. When a whole folder is created, moved or deleted, inotify reports the folder but not the files in it. In that case the client rescans `sync_dir` against the record, which costs one `stat` per unchanged file.
  - Folders on the server and on the other clients are created as files appear in them, and removed once they're empty.
  - Renaming a folder is synced as its files moving to the new path.
- **Hashing only what changed:** a file whose size and modification time match the record isn't read again; its saved hash is reused, the way git's index works. The server keeps a similar cache for its listings.
- **Pushes:** changes pushed by the server are written to a temporary file and moved in the same way.
- **Reconciliation:** this runs after every (re)connection. It's a three-way comparison of each file between the local copy, the server's copy and the saved record:

  | Situation | Result |
  |---|---|
  | Same content on both sides | nothing to do |
  | Changed (created, modified or deleted) on one side only | applied to the other side |
  | Modified on both sides | the server's version is kept and the local one becomes a conflicted copy |
  | Modified on one side, deleted on the other | the file is kept |

- **Who gets a change:** an upload or delete from the folder watcher is pushed to the user's *other* sessions. One from an explicit `upload` or `delete` command is pushed to *all* of them, including the one that issued it.

### Replication and failover

The servers form a **primary-backup** cluster. Clients only talk to the primary. For every change, the primary:
1. applies it locally;
2. copies it to every backup over a persistent **replica link**, waiting for each backup to confirm;
3. only then acknowledges the client.

So any change a client was told succeeded is on every live server. Changes are serialized by a write lock, which means backups apply them in the same order as the primary.

- **Joining:** a new server contacts any member (`Join`). If that member is a backup, it redirects the newcomer to the primary. The primary assigns an id, opens a replica link to the newcomer, compares both servers' file lists (name and hash), and sends whatever is missing or different, deleting what shouldn't be there. Finally it tells every server and client about the new member.
- **Failure detection:**
  - The primary sends a heartbeat on each replica link every 500 ms; a backup whose link fails is dropped.
  - A backup that loses its link, or hears nothing for 3 s, suspects the primary.
- **Election (bully algorithm):** the suspicious backup sends `Election` to every server with a higher id.
  - If one answers, that one takes over the election.
  - If none answers, the backup becomes the primary. It announces itself with `Coordinator` and opens replica links to everyone, which also brings any backup that missed a change up to date.
- **Ids:** the first server gets id 1000, and each newcomer gets a lower id than everyone before it. That keeps the primary at the highest id, which is the invariant the bully algorithm relies on. A backup that wrongly suspects a live primary always asks it during the election, so a second primary isn't elected. When the primary does die, the backup that joined first takes over.
- **Clients:** when a client loses its connection (closed socket, failed request, or a ping unanswered for 3 s), it tries every server it knows of, newest primary candidate first, and follows redirects. Once logged in again, it reconciles `sync_dir`, which covers anything that changed during the switch.

```mermaid
sequenceDiagram
    participant C as Client
    participant P as Primary (1000)
    participant B1 as Backup (999)
    participant B2 as Backup (998)
    C->>P: Upload notes.txt
    P->>B1: ReplPut notes.txt
    B1-->>P: Ok
    P->>B2: ReplPut notes.txt
    B2-->>P: Ok
    P-->>C: Ok
    Note over P: crashes
    B1->>B1: replica link closed
    Note over B1: no higher id answers
    B1->>B2: Coordinator(999)
    B1->>B2: ReplicaAttach + resync
    C--xP: Login (connection refused)
    C->>B1: Login, SyncAttach, reconcile
```

### Threads and locking

- **Server:**
  - one thread per connection, plus the cluster monitor (heartbeats and suspicion);
  - `write_mutex_` serializes changes to storage and their replication;
  - each replica link has a transaction lock;
  - the cluster's own state has a mutex, which is never held while doing network I/O;
  - locks are always taken in that order.
- **Client:**
  - **main** reads commands;
  - **supervisor** owns the connection (login, reconcile, pushes, pings, reconnects);
  - **worker** syncs local changes;
  - **watcher** runs inotify.

  The command connection is used by one exchange at a time (`cmd_mutex_`). The sync state and the files in `sync_dir` are guarded by `sync_mutex_`.
- **Each connection** has a single reader. Senders hold its send lock for a whole message, or for a whole file transfer.

---

## Testing

```bash
make test
```

This runs `tests/integration.sh`, 78 checks with real servers and clients on random local ports. It generates its own certificates. Among other things, it checks:
- sync between sessions, including a 3 MB file, names with spaces and empty files;
- subfolders: nested files, renaming a folder, moving a folder in, deleting a folder;
- that each change is uploaded exactly once and never bounces back;
- every command;
- changes made while a client was offline, and conflicting edits;
- passwords: wrong or too short ones are refused, and only salted hashes are stored;
- that a client refuses a server it can't verify;
- the session limit;
- a client crash, which must leave the server idle and free the session;
- invalid logins and paths, plaintext connections and certificate-less cluster messages sent straight to the server (`tests/bad_requests.py`, needs python3);
- a server restart;
- replication of files and accounts;
- missing and stray servers flagged by `--expect`, in server logs and client terminals;
- failover, with clients moving to the new primary and passwords still checked there;
- a server joining through a backup;
- a server with a certificate from another CA being refused.

It takes about 30 seconds. Run it with `KEEP=1` to keep the logs and data directories afterwards.

The suite has also been run under ThreadSanitizer, AddressSanitizer and UndefinedBehaviorSanitizer with no reports. GCC 11's TSan needs a small shim for `pthread_cond_clockwait`, which it doesn't intercept; without it, it reports false races in `condition_variable::wait_for`.

---

## Limitations

This is a teaching prototype:

- **Accounts:** there's no way to change or reset a password, or to delete an account, short of editing `<data>/accounts/` on every server. Anyone who can reach the server can create a new account.
- **Certificates:** one certificate authority and, by default, one server certificate shared by every server. There is no revocation, so a leaked server key means re-creating the authority.
- **Folders:** empty folders aren't synced; only files (and the folders that contain them) are.
- **Transfers:** files are sent whole. There are no deltas and no resuming of interrupted transfers; a transfer that fails is simply redone.
- **Replication cost:** replication is synchronous, so a slow backup slows uploads down. While a backup is being brought up to date (joining, or after an election), changes wait.
- **Partitions:** the design handles crashes, not network partitions. If the network splits the servers into groups that can't see each other, each group can elect its own primary, and their files diverge. Use [`--expect`](#expected-servers) to be warned the moment that happens. A partial fix is easy (a primary steps down when it can't reach a majority, and an election needs a majority), but on its own that can still lose a change a client was told succeeded. Doing it properly takes a consensus protocol such as Raft: numbered terms, a replicated log, and only the most up-to-date server being allowed to win an election.
- **Sessions:** sessions live in memory on the primary. After a failover, clients log in again (automatically); files aren't affected.
