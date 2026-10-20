#!/usr/bin/env bash
# End-to-end tests: starts real servers and clients on localhost and checks
# that files end up where they should. Linux only (WSL works); needs openssl.
#
#   tests/integration.sh          run everything
#   KEEP=1 tests/integration.sh   keep the work directory (logs, data) afterwards
#
# Clients read their password and then their commands from a FIFO, so tests
# can type into them.

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
make -C "$ROOT" -s all || exit 1
SERVER="$ROOT/build/server"
CLIENT="$ROOT/build/client"

WORK=$(mktemp -d /tmp/sisop-test.XXXXXX)
BASE_PORT=${BASE_PORT:-$((20000 + RANDOM % 20000))}
P1=$BASE_PORT P2=$((BASE_PORT + 1)) P3=$((BASE_PORT + 2)) P4=$((BASE_PORT + 3)) P5=$((BASE_PORT + 4))
CERTS=$WORK/certs
"$ROOT/tools/make-certs.sh" "$CERTS" >/dev/null || exit 1

PASS=0
FAIL=0
declare -A PIDS FDS

cleanup() {
    for pid in "${PIDS[@]}"; do kill -9 "$pid" 2>/dev/null; done
    wait 2>/dev/null
    if [ "$FAIL" -eq 0 ] && [ -z "${KEEP:-}" ]; then
        rm -rf "$WORK"
    else
        echo "logs and data kept in $WORK"
    fi
}
trap cleanup EXIT

# ------------------------------------------------------------------ helpers

pass() { PASS=$((PASS + 1)); echo "  ok    $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1"; }

# wait_for SECONDS COMMAND...: retry COMMAND until it succeeds or time runs out.
wait_for() {
    local deadline=$((SECONDS + $1))
    shift
    until "$@" 2>/dev/null; do
        [ "$SECONDS" -ge "$deadline" ] && return 1
        sleep 0.2
    done
}
# check DESCRIPTION COMMAND...: passes once COMMAND succeeds (within 20 s).
check() {
    local description=$1
    shift
    if wait_for 20 "$@"; then pass "$description"; else fail "$description"; fi
}
# check_now DESCRIPTION COMMAND...: COMMAND must succeed right away.
check_now() {
    local description=$1
    shift
    if "$@" 2>/dev/null; then pass "$description"; else fail "$description"; fi
}

has() { [ "$(cat "$1")" = "$2" ]; }              # has FILE CONTENT
gone() { [ ! -e "$1" ]; }
same() { cmp -s "$1" "$2"; }
logged() { grep -qF -- "$2" "$1"; }               # logged LOGFILE TEXT
alive() { kill -0 "${PIDS[$1]}"; }
count() { grep -c -- "$2" "$1" 2>/dev/null || true; }
cpu_ticks() { awk '{ print $14 + $15 }' "/proc/$1/stat"; }

# Fresh log for each start; earlier ones are kept in *.old.log.
rotate_log() { [ -f "$1" ] && cat "$1" >>"${1%.log}.old.log" && : >"$1"; return 0; }

# start_server NAME PORT [ARGS...]
start_server() {
    local name=$1 port=$2
    shift 2
    mkdir -p "$WORK/$name"
    rotate_log "$WORK/$name/server.log"
    (cd "$WORK/$name" && exec "$SERVER" --port "$port" --data data --certs "$CERTS" "$@" >>server.log 2>&1) &
    PIDS[$name]=$!
    wait_for 15 logged "$WORK/$name/server.log" "listening on port $port"
}

# start_client NAME USER PORT [PASSWORD]: runs in $WORK/NAME with ./sync_dir.
start_client() {
    local name=$1 user=$2 port=$3 password=${4:-$2-pass1}
    mkdir -p "$WORK/$name"
    rotate_log "$WORK/$name/client.log"
    rm -f "$WORK/$name/in"
    mkfifo "$WORK/$name/in"
    (cd "$WORK/$name" && exec "$CLIENT" "$user" 127.0.0.1 "$port" --ca "$CERTS/ca.pem" <in >>client.log 2>&1) &
    PIDS[$name]=$!
    local fd
    exec {fd}>"$WORK/$name/in"
    FDS[$name]=$fd
    echo "$password" >&"$fd"
    wait_for 10 logged "$WORK/$name/client.log" "Connected to"
}

# one_shot_client NAME USER PORT PASSWORD: logs in and quits right away.
one_shot_client() {
    mkdir -p "$WORK/$1"
    (cd "$WORK/$1" && printf '%s\n' "$4" | timeout 15 "$CLIENT" "$2" 127.0.0.1 "$3" --ca "$CERTS/ca.pem" >client.log 2>&1)
}

type_in() { echo "$2" >&"${FDS[$1]}"; }           # type_in CLIENT COMMAND

stop_client() {
    type_in "$1" exit
    wait_for 10 bash -c "! kill -0 ${PIDS[$1]}"
    eval "exec ${FDS[$1]}>&-"
}

kill_process() { kill -9 "${PIDS[$1]}" 2>/dev/null; wait "${PIDS[$1]}" 2>/dev/null; }

A=$WORK/a/sync_dir
B=$WORK/b/sync_dir
S1=$WORK/s1/data/users/alice

# -------------------------------------------------------------------- tests

echo "== basic sync between two sessions"
start_server s1 "$P1" || { echo "server did not start"; cat "$WORK/s1/server.log"; exit 1; }
start_client a alice "$P1" || { echo "client did not connect"; cat "$WORK/a/client.log"; exit 1; }
start_client b alice "$P1"

echo hello >"$A/hello.txt"
check "a new file reaches the other session" has "$B/hello.txt" hello
check "... and the server" has "$S1/hello.txt" hello

head -c 3000000 /dev/urandom >"$WORK/big.tmp"
mv "$WORK/big.tmp" "$A/big.bin"
check "a 3 MB file arrives intact in the other session" same "$A/big.bin" "$B/big.bin"
check "... and on the server" same "$A/big.bin" "$S1/big.bin"

echo v2 >"$B/hello.txt"
check "an edit in the other session comes back" has "$A/hello.txt" v2

rm "$A/hello.txt"
check "a deletion reaches the other session" gone "$B/hello.txt"
check "... and the server" gone "$S1/hello.txt"

echo spaced >"$A/my notes.txt"
: >"$A/empty.txt"
check "names with spaces work" has "$B/my notes.txt" spaced
check "empty files work" bash -c "[ -f '$B/empty.txt' ] && [ ! -s '$B/empty.txt' ]"

echo "== subfolders"
mkdir -p "$A/docs/notes"
echo nested >"$A/docs/notes/n.txt"
check "files in subfolders are synced" has "$B/docs/notes/n.txt" nested
check "... into the same folders on the server" has "$S1/docs/notes/n.txt" nested
echo deep >"$B/docs/notes/deep.txt"
check "subfolder changes flow both ways" has "$A/docs/notes/deep.txt" deep
mv "$A/docs" "$A/documents"
check "renaming a folder moves its files everywhere" has "$B/documents/notes/n.txt" nested
check "... and the old folder disappears" gone "$B/docs"
check "... on the server too" bash -c "[ -f '$S1/documents/notes/deep.txt' ] && [ ! -e '$S1/docs' ]"
mkdir -p "$WORK/outside/pics"
echo p1 >"$WORK/outside/pics/p1.txt"
mv "$WORK/outside/pics" "$A/pics"
check "a folder moved in with files already inside is synced" has "$B/pics/p1.txt" p1
rm -r "$A/documents"
check "deleting a folder deletes it everywhere" gone "$B/documents"
check "... including on the server" gone "$S1/documents"

echo "== no duplicate uploads, no echo loops"
before=$(count "$WORK/s1/server.log" "stored alice/")
echo once >"$A/once.txt"
check "the file is synced" has "$B/once.txt" once
sleep 3
after=$(count "$WORK/s1/server.log" "stored alice/")
check_now "it was uploaded exactly once (and not bounced back)" [ "$((after - before))" -eq 1 ]

echo "== commands"
echo external >"$WORK/external.txt"
type_in a "upload $WORK/external.txt"
check "upload puts the file in every sync_dir" has "$B/external.txt" external
check "... including the uploader's own" has "$A/external.txt" external

type_in b "download big.bin"
check "download copies the file to the current directory" same "$WORK/b/big.bin" "$A/big.bin"
type_in b "download pics/p1.txt"
check "download works for files in subfolders" has "$WORK/b/p1.txt" p1

type_in a "list_server"
check "list_server shows the files" bash -c "grep -A20 'Files on the server' '$WORK/a/client.log' | grep -q 'pics/p1.txt'"

type_in a "delete external.txt"
check "delete removes the file from the server" gone "$S1/external.txt"
check "... and from every sync_dir" bash -c "[ ! -e '$A/external.txt' ] && [ ! -e '$B/external.txt' ]"
type_in a "delete pics/p1.txt"
check "delete works in subfolders (and empty folders go away)" bash -c "[ ! -e '$B/pics' ] && [ ! -e '$S1/pics' ]"

type_in a "download nope.txt"
check "downloading a missing file reports an error" logged "$WORK/a/client.log" "no such file on the server"
check_now "... and the server keeps running" alive s1

echo "== changes made while offline"
stop_client a
echo made-offline >"$A/offline.txt"
rm "$A/once.txt"
echo from-b >"$B/while_away.txt"
check "(the other session's file reaches the server)" has "$S1/while_away.txt" from-b
start_client a alice "$P1"
check "a file created offline is uploaded" has "$B/offline.txt" made-offline
check "a file deleted offline is deleted everywhere" bash -c "[ ! -e '$B/once.txt' ] && [ ! -e '$S1/once.txt' ]"
check "a file created elsewhere meanwhile is downloaded" has "$A/while_away.txt" from-b
check_now "files that were already there are kept" same "$A/big.bin" "$S1/big.bin"

echo "== conflicting edits"
echo original >"$A/conflict.txt"
check "(the file is synced first)" has "$B/conflict.txt" original
stop_client b
echo from-a >"$A/conflict.txt"
check "(a's edit reaches the server)" has "$S1/conflict.txt" from-a
echo from-b >"$B/conflict.txt"
start_client b alice "$P1"
check "the server's version wins the name" has "$B/conflict.txt" from-a
check "the other edit is kept as a conflicted copy" bash -c "[ \"\$(cat '$B'/conflict\ \(conflicted\ copy*.txt)\" = from-b ]"
check "... which is synced to the other session too" bash -c "[ \"\$(cat '$A'/conflict\ \(conflicted\ copy*.txt)\" = from-b ]"

echo "== passwords and encryption"
one_shot_client wrong alice "$P1" not-the-password
check_now "a wrong password is refused" logged "$WORK/wrong/client.log" "wrong password"
one_shot_client short newbie "$P1" abc
check_now "a too short password is refused" logged "$WORK/short/client.log" "passwords must have"
check_now "passwords are not stored in plain text" bash -c "! grep -rq 'alice-pass1' '$WORK/s1/data'"
check_now "... only salted hashes" grep -q '^pbkdf2-sha256\$' "$WORK/s1/data/accounts/alice"
"$ROOT/tools/make-certs.sh" "$WORK/other-certs" >/dev/null
mkdir -p "$WORK/other"
(cd "$WORK/other" && printf 'alice-pass1\n' | timeout 15 "$CLIENT" alice 127.0.0.1 "$P1" --ca "$WORK/other-certs/ca.pem" >client.log 2>&1)
check_now "a client refuses a server whose certificate it can't verify" logged "$WORK/other/client.log" "Could not connect"

echo "== sessions and crashes"
one_shot_client c alice "$P1" alice-pass1
check_now "a third session of the same user is refused" logged "$WORK/c/client.log" "already has 2 active sessions"

kill_process b
sleep 1
t0=$(cpu_ticks "${PIDS[s1]}")
sleep 3
t1=$(cpu_ticks "${PIDS[s1]}")
check_now "the server stays idle after a client crashes (used $((t1 - t0)) ticks in 3 s)" [ "$((t1 - t0))" -lt 30 ]
start_client d alice "$P1"
check_now "the crashed client's session was freed" logged "$WORK/d/client.log" "Connected to"
check "the new session gets the files" same "$WORK/d/sync_dir/big.bin" "$S1/big.bin"

echo "== bad input sent straight to the server"
if command -v python3 >/dev/null; then
    if python3 "$ROOT/tests/bad_requests.py" "$P1" "$CERTS/ca.pem" >"$WORK/bad_requests.log" 2>&1; then
        pass "invalid logins, paths and messages are refused; plaintext and certificate-less cluster messages too"
    else
        fail "invalid requests: $(tr '\n' ';' <"$WORK/bad_requests.log")"
    fi
    check_now "nothing was written outside the user's directory" bash -c "[ ! -e '$WORK/s1/data/escape.txt' ] && [ ! -e '$WORK/s1/data/users/escape.txt' ]"
    check_now "the server survives malformed messages" alive s1
    check_now "the user's files are intact" same "$A/big.bin" "$S1/big.bin"
else
    echo "  skip  python3 not found"
fi

EXPECT="127.0.0.1:$P1,127.0.0.1:$P2,127.0.0.1:$P3"

echo "== server restart"
kill_process s1
start_server s1 "$P1" --expect "$EXPECT"
check_now "the server can listen on the same port again right away" alive s1
check "clients reconnect by themselves" bash -c "[ \$(grep -c 'Connected to' '$WORK/a/client.log') -ge 2 ]"
echo after-restart >"$A/restart.txt"
check "sync works again" has "$WORK/d/sync_dir/restart.txt" after-restart

echo "== expected servers (--expect)"
check "the primary flags expected servers that aren't running" logged "$WORK/s1/server.log" "127.0.0.1:$P2 is not running"
check "... and tells the clients" logged "$WORK/a/client.log" "[cluster] warning: expected server 127.0.0.1:$P2 is not running"
start_server lone "$P3" # a cluster of its own, as if started without --join
check "a server running on its own is flagged as a separate primary" logged "$WORK/s1/server.log" "127.0.0.1:$P3 is running as a separate primary"
check "... clients see that too" logged "$WORK/d/client.log" "127.0.0.1:$P3 is running as a separate primary"
kill_process lone

echo "== replication and failover"
start_server s2 "$P2" --join "127.0.0.1:$P1" --expect "$EXPECT"
start_server s3 "$P3" --join "127.0.0.1:$P1" --expect "$EXPECT"
check "once every expected server is up, clients are told" logged "$WORK/a/client.log" "[cluster] all 3 expected servers are up"
check "a new backup receives the existing files" same "$WORK/s2/data/users/alice/big.bin" "$S1/big.bin"
check "... every backup does" same "$WORK/s3/data/users/alice/big.bin" "$S1/big.bin"
check "... and the accounts" same "$WORK/s2/data/accounts/alice" "$WORK/s1/data/accounts/alice"
echo replicated >"$A/replicated.txt"
mkdir -p "$A/deep/er"
echo nested >"$A/deep/er/file.txt"
check "new files are replicated to the backups" bash -c "[ \"\$(cat '$WORK/s2/data/users/alice/replicated.txt')\" = replicated ] && [ \"\$(cat '$WORK/s3/data/users/alice/replicated.txt')\" = replicated ]"
check "... files in subfolders too" has "$WORK/s3/data/users/alice/deep/er/file.txt" nested
rm "$A/restart.txt"
check "deletions are replicated too" bash -c "[ ! -e '$WORK/s2/data/users/alice/restart.txt' ] && [ ! -e '$WORK/s3/data/users/alice/restart.txt' ]"

mkdir -p "$WORK/rogue"
(cd "$WORK/rogue" && timeout 40 "$SERVER" --port "$P5" --data data --certs "$WORK/other-certs" --join "127.0.0.1:$P1" >server.log 2>&1)
check_now "a server without a certificate from the cluster's CA can't join" logged "$WORK/rogue/server.log" "could not join"
check_now "... and got no files" gone "$WORK/rogue/data/users/alice"

kill_process s1
check "the first backup to join becomes the primary" logged "$WORK/s2/server.log" "is now the primary"
check "client a moves to the new primary" logged "$WORK/a/client.log" "Connected to 127.0.0.1:$P2"
check "client d moves too" logged "$WORK/d/client.log" "Connected to 127.0.0.1:$P2"
check "the new primary flags the server that went down, to its clients too" logged "$WORK/a/client.log" "expected server 127.0.0.1:$P1 is not running"
echo after-failover >"$A/failover.txt"
check "sync works after the failover" has "$WORK/d/sync_dir/failover.txt" after-failover
check "the new primary replicates to the remaining backup" has "$WORK/s3/data/users/alice/failover.txt" after-failover
one_shot_client wrong2 alice "$P2" not-the-password
check_now "the new primary knows the password (accounts were replicated)" logged "$WORK/wrong2/client.log" "wrong password"

start_server s4 "$P4" --join "127.0.0.1:$P3"
check "a server joining through a backup is redirected and gets every file" has "$WORK/s4/data/users/alice/failover.txt" after-failover
check_now "only one primary" [ "$(grep -l 'is now the primary\|started a new cluster' "$WORK"/s[234]/server.log | wc -l)" -eq 1 ]

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
