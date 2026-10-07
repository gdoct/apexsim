#!/bin/sh
# Seed the editable config into the data volume on first start, so the
# dashboard's Config view can save it and the edits survive a new container.
set -e
if [ ! -f /data/server.toml ]; then
  cp /etc/apexsim/server.toml /data/server.toml
fi
if [ "$#" -eq 0 ]; then
  set -- --config /data/server.toml
fi
exec apexsim-server "$@"
