# ApexSim server image.  Build context is this folder (server/):
#   docker build -t apexsim-server server
# Content (cars, tracks incl. generated sidecars, showcase) is NOT baked in:
# it is mostly gitignored build output, so mount it at /content (see
# docker-compose.yml).

FROM rust:1-bookworm AS build
WORKDIR /src
# Dependency layer: build with stub sources so edits to src/ keep the cache.
COPY Cargo.toml Cargo.lock ./
RUN mkdir -p src/bin benches \
 && echo 'fn main() {}' > src/main.rs \
 && echo 'fn main() {}' > src/bin/apexsim_replay.rs \
 && echo 'fn main() {}' > benches/physics_tick.rs \
 && cargo build --release --bins \
 && rm -rf src benches target/release/apexsim* target/release/deps/apexsim*
COPY src ./src
COPY benches ./benches
RUN cargo build --release --bins

FROM debian:bookworm-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --uid 10001 --home /data apexsim \
 && mkdir -p /data /content && chown apexsim /data
COPY --from=build /src/target/release/apexsim-server /src/target/release/apexsim-replay /usr/local/bin/
COPY server.docker.toml /etc/apexsim/server.toml

# records/, replays/ and showcase/ resolve against the working directory.
WORKDIR /data
USER apexsim
VOLUME ["/data"]
# TCP lobby, UDP telemetry/input, HTTP health/metrics
EXPOSE 9000/tcp 9001/udp 9002/tcp
HEALTHCHECK --interval=30s --timeout=3s --start-period=60s \
  CMD curl -fs http://127.0.0.1:9002/health || exit 1
ENTRYPOINT ["apexsim-server"]
CMD ["--config", "/etc/apexsim/server.toml"]
