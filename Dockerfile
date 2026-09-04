# NX-Nexus build environment.
#
# The devkitPro image already provides devkitA64, libnx and switch-tools, with
# DEVKITPRO=/opt/devkitpro set in the environment. This image is multi-arch
# (linux/amd64 and linux/arm64), so it runs natively on both x86-64 and arm64
# build hosts with no emulation.
#
# Build the .nro:
#   docker build -t nx-nexus .
#   docker create --name nx-nexus-out nx-nexus
#   docker cp nx-nexus-out:/project/NX-Nexus.nro .
#   docker rm nx-nexus-out
#
# Iterate against the working tree instead (no rebuild of the image, and
# artifacts land straight in your checkout):
#   docker run --rm -v "$PWD:/project" -w /project devkitpro/devkita64:latest make -j"$(nproc)"
#
# Pin the tag rather than tracking latest once the project has a release, so a
# toolchain bump never turns up as a surprise build break.
FROM devkitpro/devkita64:latest

# Fail early and loudly if the base image ever stops providing the toolchain.
RUN test -n "$DEVKITPRO" \
    && test -d "$DEVKITPRO/libnx" \
    && "$DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc" --version

WORKDIR /project

# Copy the build inputs only. Everything else (docs, CI config, git metadata)
# is irrelevant to the compiler and would only bust the layer cache.
COPY Makefile ./
COPY include/ ./include/
COPY src/ ./src/

RUN make -j"$(nproc)" && ls -l NX-Nexus.nro

# Default command re-runs the build, which makes the image useful as a
# throwaway builder when mounted over a working tree.
CMD ["make", "-j4"]
