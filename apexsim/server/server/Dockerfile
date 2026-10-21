# ApexSim server image. The build context is the folder holding the roots
# (the server builds against ../sim, ../protocol and ../../../apexsim-lib/formats),
# filtered to the server, the simulation, the protocol and the formats crate by
# Dockerfile.dockerignore:
#   docker build -f apexsim/server/server/Dockerfile -t apexsim-server .
# Content (cars, tracks incl. generated sidecars, showcase) is NOT baked in:
# it is mostly gitignored build output, so mount it at /content (see
# docker-compose.yml).

FROM rust:1-bookworm AS build
WORKDIR /src/apexsim/server/server
COPY apexsim-lib/formats /src/apexsim-lib/formats
COPY apexsim/server/sim /src/apexsim/server/sim
COPY apexsim/server/protocol /src/apexsim/server/protocol
# Dependency layer: build with stub sources so edits to src/ keep the cache.
COPY apexsim/server/server/Cargo.toml apexsim/server/server/Cargo.lock apexsim/server/server/build.rs ./
RUN mkdir -p src/bin benches \
 && echo 'fn main() {}' > src/main.rs \
 && echo 'fn main() {}' > src/bin/apexsim_replay.rs \
 && echo 'fn main() {}' > benches/physics_tick.rs \
 && cargo build --release --bins \
 && rm -rf src benches target/release/apexsim* target/release/deps/apexsim*
COPY apexsim/server/server/src ./src
COPY apexsim/server/server/benches ./benches
RUN cargo build --release --bins

FROM debian:bookworm-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --uid 10001 --home /data apexsim \
 && mkdir -p /data /content && chown apexsim /data
COPY --from=build /src/apexsim/server/server/target/release/apexsim-server /src/apexsim/server/server/target/release/apexsim-replay /usr/local/bin/
COPY apexsim/server/server/server.docker.toml /etc/apexsim/server.toml
COPY --chmod=755 apexsim/server/server/docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh

# records/, replays/ and showcase/ resolve against the working directory.
WORKDIR /data
USER apexsim
VOLUME ["/data"]
# TCP lobby, UDP telemetry/input, HTTP health/metrics, web dashboard (HTTP, HTTPS)
EXPOSE 9000/tcp 9001/udp 9002/tcp 9003/tcp 9004/tcp
HEALTHCHECK --interval=30s --timeout=3s --start-period=60s \
  CMD curl -fs http://127.0.0.1:9002/health || exit 1
# The config lives in /data/server.toml (seeded from /etc/apexsim on first start).
ENTRYPOINT ["docker-entrypoint.sh"]
CMD []
