################################
# Busybox stage
FROM --platform=linux/riscv64 riscv64/busybox:1.37.0-musl AS busybox-stage

################################
# Rootfs stage
FROM --platform=linux/riscv64 riscv64/alpine:3.21.0 AS toolchain-stage

# Update and install development packages
RUN apk update && \
    apk upgrade && \
    apk add build-base pkgconf git wget

# Build other packages inside /root
WORKDIR /root

################################
# Download packages
FROM --platform=linux/riscv64 riscv64/alpine:3.21.0 AS rootfs-stage

# Add Cartesi repository key
ADD --chmod=644 https://edubart.github.io/linux-packages/apk/keys/cartesi-apk-key.rsa.pub /etc/apk/keys/cartesi-apk-key.rsa.pub

# Update packages
RUN echo "@testing https://dl-cdn.alpinelinux.org/alpine/edge/testing" >> /etc/apk/repositories && \
    echo "https://edubart.github.io/linux-packages/apk/stable" >> /etc/apk/repositories && \
    apk update && \
    apk upgrade

# Install development utilities
RUN apk add \
    bash bash-completion \
    python3 \
    py3-requests \
    git \
    make \
    musl-dev \
    cartesi-machine-guest-tools

# Overwrite busybox
COPY --from=busybox-stage /bin/busybox /bin/busybox

COPY skel /
RUN rm -rf /var/cache/apk && \
    rm -f /usr/lib/*.a && \
    ln -sf python3 /usr/bin/python