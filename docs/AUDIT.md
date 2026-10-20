# Code Audit

Audit of the SisOp2 "bootleg Dropbox" prototype, done in October 2026.

It covers two snapshots of the code:

- **`main`**: what is checked out today (`2022f2f`).
- **`final`**: the `origin/final` branch (`4e374e0`, 2023-03-23). This is the most complete version of the project. See [How the branches got here](#how-the-branches-got-here).

Each finding is marked:

- **Verified**: it was reproduced by building the code and running it under Linux (WSL Ubuntu 22.04, g++ 11.4).
- **Reading**: it was found by reading the code, and the code path is unambiguous.

---

## TL;DR

1. **`main` does not compile.** The April 2026 merge combined two incompatible versions of the packet code. One file needs a small fix (see [B1](#b1-main-does-not-compile)).
2. **Two bugs let any client destroy the server.** `delete ..` wipes the server's whole working directory, and `download <missing-file>` crashes the server process. Both are in `main` and `final`.
3. **On `main`, two users logged in at once get their files mixed together**, because every server thread shares one global `user` buffer. `final` already fixes this.
4. **The client wipes your local `sync_dir` every time it starts.**
5. **PR #4** (`LC--reconnect`, client reconnection) is half-finished. The reconnection path cannot work as written, and its heartbeat thread corrupts the main connection. It is already contained in both `main` and `final`, so the PR itself should probably just be closed (see [PR #4 review](#pr-4-review)).

---

## Summary table

| # | Severity | Bug | `main` | `final` | How found |
|---|---|---|:-:|:-:|---|
| [B1](#b1-main-does-not-compile) | Blocker | `main` doesn't compile | ✗ | ok | Verified |
| [S1](#s1-delete--wipes-the-server) | Critical | `delete ..` recursively deletes the server's working dir | ✗ | ✗ | Verified |
| [S2](#s2-download-of-a-missing-file-crashes-the-server) | Critical | `download <missing>` segfaults the server | ✗ | ✗ | Verified |
| [S3](#s3-path-traversal-via-download-and-usernames) | Critical | Path traversal via `download ../x` and via usernames | ✗ | ✗ | Reading |
| [C1](#c1-all-sessions-share-one-global-user-buffer) | High | All sessions share one global `user` buffer | ✗ | ok | Verified |
| [C2](#c2-client-deletes-local-sync_dir-on-startup) | High | Client deletes local `sync_dir` on startup | ✗ | ✗ | Verified |
| [C3](#c3-heartbeat-thread-corrupts-the-command-connection) | High | Heartbeat thread corrupts the command connection | ✗ | ✗* | Verified |
| [C4](#c4-dead-client--server-thread-spins-at-100-cpu-forever) | High | Dead client: server thread spins at 100% CPU forever | ✗ | ✗ | Verified |
| [C5](#c5-writing-to-a-dead-socket-kills-the-whole-process-sigpipe) | High | Writing to a dead socket kills the whole process (SIGPIPE) | ✗ | ✗ | Reading |
| [C6](#c6-get_sender_sync_sock-only-works-for-the-first-user) | High | `get_sender_sync_sock` only works for the first user | ✗ | ✗ | Reading |
| [C7](#c7-sync-echo-loops-between-clients) | High | Sync echo loops between clients | ✗ | ✗ | Reading |
| [C8](#c8-every-new-file-is-uploaded-twice-sometimes-half-written) | Medium | Every new file is uploaded twice, sometimes half-written | ✗ | ✗ | Verified |
| [C9](#c9-race-conditions-in-loginmanager) | Medium | Race conditions in `LoginManager` | ✗ | ✗ | Reading |
| [C10](#c10-logout-dereferences-end) | Medium | `Logout` dereferences `end()` | ✗ | ✗ | Reading |
| [C11](#c11-client-busy-loops-burn-two-cpu-cores) | Medium | Client busy-loops burn two CPU cores | ✗ | ✗ | Reading |
| [C12](#c12-thread-arguments-point-at-stack-variables-that-get-reused) | Medium | Thread args point at reused stack variables | ✗ | ✗ | Reading |
| [N1](#n1-ports-and-hostnames-dont-really-work) | Medium | Custom ports and hostnames don't work | ✗ | ✗ | Reading / Verified |
| [N2](#n2-server-restarts-hang-for-up-to-a-minute) | Low | Server restarts hang for up to a minute (no `SO_REUSEADDR`) | ✗ | ✗ | Verified |
| [N3](#n3-sendmessage-over-reads-its-input) | Low | `sendMessage` over-reads its input | ✗ | ✗ | Reading |
| [R1](#r1-receive_list_of_servers-only-reads-one-server) | High | `receive_list_of_servers` only reads one server | ✗ | ✗ | Reading |
| [R2](#r2-every-server-elects-itself-on-main) | High | Every server elects itself (`main`) | ✗ | ok | Reading |
| [R3](#r3-remove_from_server_list-erases-the-wrong-element-on-main) | Medium | `remove_from_server_list` erases the wrong element (`main`) | ✗ | ok | Reading |
| [R4](#r4-backups-never-learn-about-users-so-failover-cant-restore-sessions) | High | Backups never learn about users, so failover can't restore sessions | ✗ | ✗ | Reading |
| [R5](#r5-pthread_cancel-can-deadlock-sends) | Medium | `pthread_cancel` can deadlock all sends | ✗ | ✗ | Reading |
| [H1](#housekeeping) | — | Broken makefiles, committed binaries and test files, dead code | — | — | Reading |

✗ = bug present, ok = not present. \* `final` has a different variant of C3; see its section.

---

## How the branches got here

```
2023-03-08  9e6758a ──┬── multiple-server ── … ── 88d151c "upload replication working" (03-23)
                      │                                   │
                      └── LC--reconnect (Augusto) ─ 9d4d550 ──┴── merged into `final` (03-23), + 4e374e0
                                         │
2023-02-12  main (single-server version) │
     …                                   │
2026-04-03  93d7fc7 "Possible fix…"      │
2026-04-03  4d41f4f merge LC--reconnect ─┘   ← this is where `main` broke
```

- On 2023-03-23 the reconnection branch was merged together with the multi-server work into **`final`**. That is the real end state of the project, and it compiles.
- In April 2026, `LC--reconnect` was merged into `main`. That branch had forked on 2023-03-08, two weeks before the multi-server work was finished. So `main` received an **old** copy of the server, without the replication and bug fixes that exist on `final`.
- `main` had also just received `93d7fc7`, which serializes `file_byte_size` and `user` fields. Those fields existed in `main`'s old `PACKET` struct but not in the branch's struct. The merge kept the branch's header and `main`'s `.cpp`, so the build broke.
- **Nothing is lost.** Everything is on `origin/final`.

**Recommendation:** treat `final` as the source of truth. If you want `main` to be the finished project, reset or fast-forward it to `final` (and re-apply the README/docs). Don't fix bugs on `main` first.

---

## PR #4 review

[PR #4](https://github.com/izaias-saturnino/SisOp2/pull/4), "Lc reconnect + multiple_servers", comes from `LC--reconnect` and targets `teste_merge`. It contains two commits by Augusto (e24e04c and 9d4d550), with no description.

**What it tries to do:** when the primary server dies and a backup wins the election, clients should reconnect to the new primary. The pieces are:

- The server stores each client's address at login (`socketAddress1/2`).
- The client runs a heartbeat thread (`verificaServer`). When the heartbeat fails, the client listens on TCP port 2000 (`waitForReconnection`).
- The new primary connects back to every known client (`reconnectToClients`).

**What's good:**

- It replaced the old thread-argument race in the server's accept loop: `&newSockfd` was a stack variable reused by the next `accept`. The PR gives each connection its own `malloc`'d `parametros` struct instead. That is a real fix, although the struct is never `free`d.

**Why it can't work as written:**

1. **The heartbeat corrupts the connection (C3).** `verificaServer` reads from the same socket the main thread uses for replies, so the two threads steal each other's bytes. It also reads `sizeof(PACKET)` bytes into `&pack`, which is the address of a pointer variable (8 bytes). That is a stack buffer overflow. Finally, the server on this branch never answers `MENSAGEM_VERIFICACAO`, so the heartbeat read just blocks until some other reply arrives and gets eaten.
2. **`reconnectToClients` connects the wrong socket.** It creates a fresh socket (`sockfd`) and then calls `connect()` on `it.socketClient1`, a file descriptor number from the old server's accepted connections. That number is meaningless here. The fresh socket is leaked.
3. **The new primary doesn't know any clients.** Logins are never replicated to backups (R4), so `listaDeUsuarios` is empty on the new primary and nobody gets reconnected.
4. **The first session's address is never stored.** `criarNovoUsuario` (first login of a new user) doesn't set `socketAddress1`, so even with a populated list, the first session would be reconnected to a garbage address.
5. **The client binds the listener to the server's IP.** `waitForReconnection` binds port 2000 to the *server's* address. On two machines, `bind` fails and the client calls `exit(0)`, so the client quits when the server dies. `final` fixes this with a 4th CLI argument (`<client_ip>`).
6. **Only one client per machine.** Port 2000 is hard-coded, so only one client per machine can wait for reconnection, and the backup has no way to know a client's port.
7. **There's no reconnect handshake.** After `accept`, the client never re-logs in or re-opens its sync socket, and the server's new connection isn't attached to a `ThreadClient`.

**Recommendation:** close PR #4 as superseded. Its commits are already in both `final` and `main`, and merging it into `teste_merge` would re-introduce the older server code. If reconnection matters, a simpler design is: on heartbeat failure, the *client* retries connecting to a list of known servers and logs in again. That avoids needing clients to listen at all.

---

## Findings

Paths and line numbers refer to `main` unless noted.

### B1. `main` does not compile

`common/common.cpp:10-46` serializes `pkt->file_byte_size` and `pkt->user`, but `PACKET` in `common/common.hpp:60-64` only has `type`, `seqn` and `_payload`. g++ reports 8 errors.

**Fix:** neither field is used anywhere else on `main` (file size travels in its own `MENSAGEM_ENVIO_TAMANHO_ARQUIVO` packet). Delete the two `file_byte_size` blocks and the two `user` blocks from `serialize`/`deserialize`, and keep the `memcpy` style from `93d7fc7`. That style is better than the old one: the old byte-pointer code only wrote the low byte of `type` and `seqn`.

### S1. `delete ..` wipes the server

`ThreadClient`, `MENSAGEM_DELETAR_NO_SERVIDOR` (`server/server.cpp:752-759`), builds `"./" + user + "/" + getFileName(payload)` and passes it to `delete_file`, which calls `std::filesystem::remove_all`.

`getFileName("..")` returns `..`, so the path becomes `./alice/..`, which is the server's working directory. `remove_all` deletes it recursively.

**Verified:** a client typing `delete ..` deleted every user's folder, the log, and the server binary.

The server then broadcasts `MENSAGEM_DELETAR_NOS_CLIENTES ".."` to every synced client. `handle_updates` does `remove_all("./sync_dir/..")` on each one, which wipes each client's working directory too. That client path was not separately run.

**Fix:**

- Reject names that are empty, `.`, `..`, or contain `/`.
- Use `fs::remove` (not `remove_all`) for single files.
- After building a path, check that `fs::weakly_canonical(path)` is inside the user's directory.

### S2. Download of a missing file crashes the server

`sendFile` (`common/common.cpp:270`) calls `sendMessage(nullptr, MENSAGEM_FALHA_ENVIO, sock)` when the file can't be opened. `sendMessage` then does `memcpy(pkt._payload, nullptr, 256)`, which segfaults.

**Verified:** `download nope.txt` from any client kills the server process. The same path runs whenever the initial sync or propagation hits a file that disappeared in the meantime.

**Fix:** `sendMessage((char*)"", …)`, or make `sendMessage` accept `nullptr` and zero the payload in that case.

### S3. Path traversal via `download` and usernames

- `MENSAGEM_DOWNLOAD_FROM_SERVER` (`server/server.cpp:777`) uses the raw payload, not `getFileName`. So `download ../bob/secret.txt` reads another user's file, and `download ../../etc/passwd` reads any file the server can read.
- The username is used directly as a directory name, and there are no passwords. Logging in as `..` or `../something` makes the server create and serve directories outside its data folder.

**Fix:** validate usernames (for example `[A-Za-z0-9_-]+`) and file names (see S1).

### C1. All sessions share one global `user` buffer

`server/server.cpp:4`: `char user[BUFFER_SIZE];` is global. Every `ThreadClient` writes it in `client_login` and reads it for every path and every `LoginManager` call.

**Verified:** with `alice` logged in and then `bob`, alice's new file was stored in `./bob/` on the server. Both logouts were logged as `bob logout`. Alice's session slot is therefore never freed, so after two more logins she's locked out ("Excedido numero de sessoes").

This predates PR #4; it was already on the March 8 base. `final` fixes it with the per-connection `SESSION` struct.

**Fix:** make `user` a local in `ThreadClient`, or port `final`'s `SESSION`.

### C2. Client deletes local `sync_dir` on startup

`client/client.cpp:143-155`: `get_sync_dir` runs automatically at startup (`command` starts as `"get_sync_dir"` in `filewatcher.cpp:23`). It calls `delete_file("./sync_dir")` and then recreates the directory.

**Verified:** a file placed in `sync_dir` before login was gone afterwards and never reached the server.

This is the opposite of what the README promises ("Extra local files are uploaded"). Any offline edits are silently destroyed.

**Fix:** don't delete. Diff the local listing against the server's (`get_folder_diference` already exists in `dir_manager.cpp` and is unused), then upload local-only files and download server-only files.

### C3. Heartbeat thread corrupts the command connection

`client/client.cpp:262-285` (`verificaServer`, from PR #4) runs once per second:

- It writes `MENSAGEM_VERIFICACAO` to `servAlive->sockfd`, the same socket the main thread uses for `list_server` and `download`.
- It calls `read()` on that socket, so it races the main thread for incoming bytes.
- It reads 260 bytes into `&pack`, an 8-byte pointer variable on its own stack. That is a stack buffer overflow.
- The server on `main` has no handler for type 7, so the heartbeat blocks until it steals part of some other reply.

**Verified:** `list_server` printed nothing (`listing ended` was never printed), and heartbeats stopped after two.

On **`final`** the server *does* answer `MENSAGEM_VERIFICACAO`, but then executes `break` (`final:server/server.cpp:750-754`). That ends `ThreadClient` for the session after the first heartbeat, so every later command goes unanswered.

**Fix:** use a separate connection for heartbeats. Alternatively, make the main thread the only reader and use `SO_RCVTIMEO` or `poll()` to detect a dead server. On the server, reply to the heartbeat and `continue`, don't `break`.

### C4. Dead client: server thread spins at 100% CPU forever

`readSocket` (`common/common.cpp:48`) loops `while (n < sizeof(PACKET))`. When the peer disconnects, `read` returns 0 forever, so the loop never exits. A return of -1 is ignored as well.

**Verified:** after a client was `kill -9`'d, its server thread used 4 CPU-seconds in 5 seconds and never stopped. Its session also stays "active", so (as in C1) the user can be locked out.

`peekSocket` has the same problem.

**Fix:** `readSocket` should return `-1` or `0` when `read` returns `<= 0`. Every caller (`ThreadClient`, `handle_updates`, `receiveFileData`, `answer_server_up`…) should treat that as "connection closed": log the session out and close the socket.

### C5. Writing to a dead socket kills the whole process (SIGPIPE)

Nothing ignores `SIGPIPE`. When the server writes to a socket whose client has gone away, the kernel sends `SIGPIPE` and the default action terminates the server. Examples:

- propagating a file to a dead client's sync socket
- the `ACK` in `answer_server_up`

`sendMessage`'s write loop also spins forever if `write` returns -1 without a signal.

**Fix:** `signal(SIGPIPE, SIG_IGN)` at startup (or `send(..., MSG_NOSIGNAL)`), and return an error from `sendMessage` when `write` fails.

### C6. `get_sender_sync_sock` only works for the first user

`login/login.cpp:151-166`: the `break;` sits outside the `if/else if`, so the loop always stops after the first user in the list. For every other user it returns -1. The server then echoes uploaded files back to the client that sent them, which feeds C7.

**Fix:** move `break` inside both branches.

### C7. Sync echo loops between clients

The client avoids re-uploading files it just downloaded by checking `latest_downloads`. But that list is cleared after every batch (`client/client.cpp:95`) and when the first sync ends.

inotify events from the download are collected by `folderchecker` every 20 ms. They can arrive in the *next* batch, after the list was cleared. The client then re-uploads the file, and the server propagates it to the other session, which re-downloads it, and so on. Commit `b3f45ed` ("Evitando loop no envio do n.gif") was likely fighting this.

Together with C6, this is a likely cause of "files stop syncing" and of the files bouncing back and forth. The `93d7fc7` serialization change wouldn't affect it.

**Fix options:**

- Keep a per-file "last synced" mtime and size (or hash), and skip uploads when nothing changed.
- Or write downloads to a temp file outside `sync_dir` and `rename()` it in, ignoring the resulting `IN_MOVED_TO` for that name once.

### C8. Every new file is uploaded twice, sometimes half-written

`filewatcher.cpp` watches `IN_CREATE` *and* `IN_MODIFY`. Creating a file fires both events, and writing a large file fires many `IN_MODIFY` events while it is still being written.

**Verified:** `echo hello > sync_dir/new.txt` produced two full uploads.

Each upload reads the file at that moment, so the server can receive a truncated copy.

**Fix:** watch `IN_CLOSE_WRITE` (and `IN_MOVED_TO`) instead of `IN_CREATE | IN_MODIFY`, and de-duplicate names within a batch.

### C9. Race conditions in `LoginManager`

- `criarNovoUsuario` pushes onto `listaDeUsuarios` under `mtx_list`. `login`, `Logout` and `activate_sync_dir` iterate it under a *different* mutex, `mtx_sessoes`. `get_active_sync_dir` and `get_sender_sync_sock` take no lock at all. A `push_back` that reallocates while another thread iterates is undefined behaviour.
- Two simultaneous first logins of the same new user both miss the lookup and create two entries.

**Fix:** use one mutex for the whole list, held for the full lookup-or-create.

### C10. `Logout` dereferences `end()`

`login/login.cpp:65`: after the loop, `(*it).sessaoAtiva1` is read even when the user was not found (`it == end()`). That is undefined behaviour, and it is reachable via C1.

### C11. Client busy-loops burn two CPU cores

- The main loop in `client.cpp:67-201` has no sleep or blocking call.
- `input()` in `filewatcher.cpp:98-107` spins while `command_complete` is true.

`command`, `command_complete`, `action`, `name` and `wait_for_first_sync` are shared between threads without synchronization or `std::atomic` (`action.size()` is checked outside the mutex). With optimizations on, the compiler may hoist these reads and hang.

**Fix:** use a `std::condition_variable` (or a pipe and `poll`) to hand commands and file events to the main thread.

### C12. Thread arguments point at stack variables that get reused

- `server.cpp:101`: `&other_server_socket` is a local inside an `if` block. It is out of scope by the time `between_server_sync` might read it.
- `server.cpp:176`: `&newSockfd` is overwritten by the next `accept()`, possibly before `answer_server_up` reads it.
- `send_election` passes `&election_servers[i]` to a timer thread, but `election_servers` is reassigned (and reallocated) by later elections.

**Fix:** pass heap-allocated copies, or pass small ints by value through `(void*)(intptr_t)fd`.

### N1. Ports and hostnames don't really work

- **Server ports:** setting `<this_port>` requires `argc > 3`, which also switches the server into backup mode. A primary can therefore only run on port 4000.
- **Client sync socket:** it always uses `DEFAULT_PORT`, ignoring the port given on the command line (`connect_to_server(*server_host)`, `client.cpp:155`).
- **Hostnames:** `connect_to_server` does `inet_aton(server_host.h_name)`. That only works when the name is already a dotted IP. `localhost` or a DNS name leaves `sin_addr` uninitialized. A failed `gethostbyname` returns `NULL`, which is then dereferenced.
- **Client argument count:** it checks `argc < 3` but reads `argv[3]`. Running `./client alice 127.0.0.1` passes `NULL` to `atoi`, which crashes. On `final` the same happens with `argv[4]`.
- **Retry window:** `connect_to_server` gives up after 10 × 20 ms. If the server isn't accepting within 200 ms, the client quits. **Verified:** this happens during N2.

### N2. Server restarts hang for up to a minute

There's no `SO_REUSEADDR`, so after a restart `bind` fails while the old port is in TIME_WAIT, and the server silently retries `bind` every second.

**Verified:** after a restart the server took 44 s to come up, and clients started during that window exited with `ERROR connecting`.

**Fix:** `setsockopt(SO_REUSEADDR)` before `bind`, and print the error instead of silently retrying.

### N3. `sendMessage` over-reads its input

`sendMessage` always copies 256 bytes from `message`. Most callers pass `""` or `c_str()` of a short string, so it reads past the end of the buffer. That is undefined behaviour, but usually harmless.

Paths of 256 characters or more are sent without a terminating `\0`. The receiver's `string(pkt._payload)` then reads past the payload.

**Fix:** zero the payload, copy `strnlen(message, BUFFER_SIZE-1)` bytes, and make sure the payload is null-terminated.

### R1. `receive_list_of_servers` only reads one server

`server/server.cpp:415`: `while(pkt.type == SERVER_ITEM);{` has a stray `;`. The loop body is empty, and the block after it runs exactly once. With more than one other server in the list, the remaining `SERVER_ITEM` packets are read as garbage by whatever reads next. The code may also hang if the first peeked packet is a `SERVER_ITEM`.

**Fix:** delete the `;`.

### R2. Every server elects itself (on `main`)

On `main`, `send_election` never sets `possible_main_server = false` when a higher-id server answers. It only cancels the timer. So every server that runs an election broadcasts `ELECTED` for itself, and several servers can believe they are primary.

`final` sets it correctly. This is part of the stale code `main` received; it isn't PR #4's doing.

### R3. `remove_from_server_list` erases the wrong element (on `main`)

`servers.erase(servers.begin()+i-1)` removes the element *before* the match, and for `i == 0` it is undefined behaviour. `final` (`66e98fd`, "fixed remove from list function") uses `+i`.

### R4. Backups never learn about users, so failover can't restore sessions

- **On `main`**, nothing replicates logins, uploads or deletes to backup servers.
- **On `final`**, the primary forwards each client's packets to every backup over a new connection, starting with `MENSAGEM_LOGIN`. But a backup's `ThreadClient` skips `client_login` when it isn't primary, so the backup's `LoginManager` stays empty.

Either way, after failover the new primary has no users, no sync sockets, and nothing for `reconnectToClients` to work with. On `final`, file *contents* are replicated, so the data survives. Sessions don't.

### R5. `pthread_cancel` can deadlock sends

`send_election` cancels the previous election thread at an arbitrary point. If it is inside `sendMessage`, it is holding the global `pkt_mtx`, and every later `sendMessage` in the process (client traffic included) blocks forever.

`std::mutex` is not cancellation-safe. Use a flag that the thread checks instead of `pthread_cancel`.

### Housekeeping

- **Broken makefiles on `final`:** `-o -std=c++17server` creates a binary named `-std=c++17server`. `-o -std=c++17 … ../client/client` tries to *link the old client binary* as an input file. The working commands are in the [README](../README.md#building).
- **Committed binaries:** `client/client`, `server/server` and `login/login` are committed Linux binaries, and they go stale immediately. Add them to `.gitignore`.
- **Committed test files:** `client/sync_dir/*`, `server/jp/*`, `iFood.pdf`, `v.mov`, `n.gif` and others are test data. They inflate the repo and confuse the initial-sync behaviour.
- **`.vscode/settings.json`** is in `.gitignore` but already tracked, so the ignore has no effect.
- **`sync.cpp`** is unused, and doesn't match the protocol (raw text commands). `create_file_from_request` and `get_folder_diference` are unused.
- **`.cpp` files included as headers:** `dir_manager.cpp` and `filewatcher.cpp` are `#include`d. Linking them twice into one binary would break the build. Use a `.hpp` + `.cpp` pair instead.
- **Debug output:** the code is full of `cout << "write12"`-style markers. A `DEBUG` macro would make the output readable.
- **Unused packet field:** `seqn` is filled from a process-wide counter shared by all sockets and never checked. It provides no ordering or reliability; TCP already does that.

---

## Suggested fix order

If you want a version that's safe to demo:

1. Start from `final`, not `main`.
2. S1, S2, S3: input validation and the `nullptr` fix. These are a few lines each.
3. C4 and C5: handle disconnects and ignore `SIGPIPE`, so one bad client can't take the server down.
4. C3 on `final`: change `break` to `continue` in the heartbeat handler.
5. C2: stop wiping `sync_dir`.
6. C6, C8, then C7: the sync-correctness group.
7. R1 and the remaining replication issues, if failover needs to work.
