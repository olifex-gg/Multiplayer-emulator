# Town server for the multiplayer Animal Crossing fork.
# Build context must be the repository root (the server vendors ENet from pc/lib/enet
# and the protocol header from net/):
#   docker build -f server/Dockerfile -t acnet-server .
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build python3 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY net/ net/
COPY pc/lib/enet/ pc/lib/enet/
COPY server/ server/
RUN cmake -S server -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build \
    && cd /build && ctest --output-on-failure

FROM debian:bookworm-slim
RUN useradd --system --uid 1000 --create-home acnet
COPY --from=build /build/acnet_server /build/acnet_cli /usr/local/bin/
USER acnet
WORKDIR /home/acnet
VOLUME ["/data"]
ENV ACNET_PORT=7777 ACNET_DATA=/data
EXPOSE 7777/udp
ENTRYPOINT ["acnet_server"]
