FROM ubuntu:24.04 AS build
ARG TARGETARCH
RUN test "$TARGETARCH" = amd64 && \
    apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates cmake g++ make python3 libopenh264-dev libjpeg-turbo8-dev && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt run.bat ./
COPY include/ include/
COPY src/ src/
COPY packaging/ packaging/
RUN cmake -S . -B /build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build /build --target bambu-bridge --parallel && \
    cp /usr/share/doc/libopenh264-dev/copyright /build/openh264-copyright.txt && \
    cp /usr/share/doc/libjpeg-turbo8-dev/copyright /build/libjpeg-turbo-copyright.txt && \
    python3 packaging/bundle.py linux_x64 /build/bambu-bridge /build/package /build/*-copyright.txt && \
    mkdir /package && tar -xzf /build/package.tar.gz -C /package

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libcurl4t64 libssl3t64 libstdc++6 tini zlib1g && \
    rm -rf /var/lib/apt/lists/* && \
    install -d -m 0700 -o 10001 -g 10001 /data
COPY --from=build /package/ /opt/bambu-bridge/
ENV HOME=/data
WORKDIR /opt/bambu-bridge
USER 10001:10001
VOLUME ["/data"]
EXPOSE 8080/tcp
ENTRYPOINT ["/usr/bin/tini", "--", "/opt/bambu-bridge/run.sh", "--http-loopback", "--config-dir", "/data"]
