FROM alpine:3.22

RUN apk add --no-cache \
    e2fsprogs \
    curl \
    tar \
    coreutils \
    protoc

WORKDIR /build
COPY build-rootfs.sh /
ENTRYPOINT ["/build-rootfs.sh"]
