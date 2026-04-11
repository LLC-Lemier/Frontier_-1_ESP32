#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

openssl req -x509 -newkey rsa:2048 -sha256 -days 3650 -nodes \
  -keyout server.key \
  -out server.crt \
  -subj "/C=RU/ST=Moscow/L=Moscow/O=Lemier/OU=Embedded/CN=Lemier"

echo "Generated: $SCRIPT_DIR/server.crt and $SCRIPT_DIR/server.key"
