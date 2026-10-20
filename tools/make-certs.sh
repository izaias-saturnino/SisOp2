#!/usr/bin/env bash
# Creates the certificates a cluster needs:
#
#   DIR/ca.pem, DIR/ca.key          a private certificate authority
#   DIR/server.pem, DIR/server.key  a server certificate signed by it
#
# usage: tools/make-certs.sh [DIR] [HOST...]
#
# DIR defaults to "certs". The server certificate is valid for localhost,
# 127.0.0.1 and every HOST given (names or IPv4 addresses): list every
# address servers will be reached at (their --host). Running it again keeps
# the existing CA and issues a new server certificate, so it can be re-run
# to add hosts.
#
# Servers need ca.pem, server.pem and server.key (all servers can share the
# same ones). Clients only need ca.pem. Keep ca.key private: anyone holding it
# can issue certificates that the cluster will accept as servers.

set -euo pipefail

DIR=${1:-certs}
shift || true
mkdir -p "$DIR"
cd "$DIR"

if [ ! -f ca.key ]; then
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
        -keyout ca.key -out ca.pem -days 3650 -subj "/CN=SisOp2 cluster CA" \
        -addext "basicConstraints=critical,CA:TRUE" \
        -addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null
fi

SAN="DNS:localhost,IP:127.0.0.1"
for host in "$@"; do
    if [[ $host =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        SAN="$SAN,IP:$host"
    else
        SAN="$SAN,DNS:$host"
    fi
done

openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
    -keyout server.key -out server.csr -subj "/CN=SisOp2 server" 2>/dev/null
# Servers also use this certificate as a client when they connect to each
# other, hence both extended key usages.
openssl x509 -req -in server.csr -CA ca.pem -CAkey ca.key -CAcreateserial \
    -out server.pem -days 825 \
    -extfile <(printf "subjectAltName=%s\nextendedKeyUsage=serverAuth,clientAuth\n" "$SAN") 2>/dev/null
rm -f server.csr ca.srl
chmod 600 ca.key server.key

echo "Certificates written to $DIR/ (server certificate valid for $SAN)"
