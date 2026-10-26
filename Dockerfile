# AirPlay 2 (shairport-sync 4.3.7 + nqptp 1.2.4) for aarch64 glibc 2.36
# Build on the Pi (native aarch64): docker build --target out -o /tmp/ap2-orion .
FROM debian:bookworm AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential autoconf automake libtool pkg-config git ca-certificates python3 \
      libasound2-dev libpopt-dev libconfig-dev libsoxr-dev \
      libssl-dev libavahi-client-dev libplist-dev libsodium-dev libgcrypt20-dev \
      libavcodec-dev libavformat-dev libavutil-dev uuid-dev xxd \
      avahi-daemon libdaemon-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt
RUN git clone --depth=1 --branch 1.2.4 https://github.com/mikebrady/nqptp.git nqptp
WORKDIR /opt/nqptp
RUN autoreconf -fi && ./configure && make -j"$(nproc)"

WORKDIR /opt
RUN git clone --depth=1 --branch 4.3.7 https://github.com/mikebrady/shairport-sync.git sps
COPY src/shairport-sync-4.3.7-*.patch /tmp/patches/
RUN python3 - <<'PY'
from pathlib import Path
import re
for p in Path("/tmp/patches").glob("*.patch"):
    out = []
    for line in p.read_text(errors="replace").splitlines(True):
        if line.startswith("--- ") or line.startswith("+++ "):
            m = re.search(r"(rtsp\.c|rtp\.c|player\.h)", line)
            if m:
                pref = line[:4]
                side = "a" if pref == "--- " else "b"
                out.append(f"{pref}{side}/{m.group(1)}\n")
                continue
        out.append(line)
    Path("/tmp").joinpath(p.name).write_text("".join(out))
PY
WORKDIR /opt/sps
RUN patch -p1 < /tmp/shairport-sync-4.3.7-player.h.patch && \
    patch -p1 < /tmp/shairport-sync-4.3.7-rtsp.c.patch && \
    patch -p1 < /tmp/shairport-sync-4.3.7-rtp.c.patch && \
    autoreconf -fi && \
    ./configure \
      --with-alsa \
      --with-soxr \
      --with-avahi \
      --with-ssl=openssl \
      --with-airplay-2 \
      --with-metadata \
      --without-libdaemon \
      CFLAGS="-Os" \
      LIBS="-luuid" && \
    make -j"$(nproc)"

COPY airplayd.c /tmp/airplayd.c
RUN gcc -O2 -o /tmp/airplayd /tmp/airplayd.c -lm && file /tmp/airplayd /opt/sps/shairport-sync /opt/nqptp/nqptp

RUN mkdir -p /out/ap2/libs && \
    cp /opt/sps/shairport-sync /out/ap2/shairport-sync-ap2 && \
    cp /opt/nqptp/nqptp /out/ap2/nqptp && \
    cp /usr/sbin/avahi-daemon /out/ap2/avahi-daemon && \
    cp /tmp/airplayd /out/airplayd && \
    printf '1.0-orion (shairport-sync 4.3.7 AirPlay 2 + nqptp 1.2.4, aarch64)\n' > /out/VERSION && \
    python3 - <<'PY'
import os, subprocess, shutil
bins = [
    "/out/ap2/shairport-sync-ap2",
    "/out/ap2/nqptp",
    "/out/ap2/avahi-daemon",
    "/out/airplayd",
]
skip = {
    "libc.so.6", "libm.so.6", "libpthread.so.0", "librt.so.1", "libdl.so.2",
    "libresolv.so.2", "ld-linux-aarch64.so.1", "libgcc_s.so.1", "libasound.so.2",
}
seen = set()
libs = []
def ldd(path):
    out = subprocess.check_output(["ldd", path], text=True, errors="replace")
    for line in out.splitlines():
        parts = line.split()
        if "=>" in parts:
            p = parts[parts.index("=>") + 1]
            if p.startswith("/") and os.path.isfile(p):
                yield p
        elif parts and parts[0].startswith("/") and os.path.isfile(parts[0]):
            yield parts[0]
for b in bins:
    for p in ldd(b):
        name = os.path.basename(p)
        if name in skip or name in seen:
            continue
        seen.add(name)
        libs.append(p)
# one extra pass for newly copied libs
again = True
while again:
    again = False
    for p in list(libs):
        try:
            for q in ldd(p):
                name = os.path.basename(q)
                if name in skip or name in seen:
                    continue
                seen.add(name)
                libs.append(q)
                again = True
        except subprocess.CalledProcessError:
            pass
dst = "/out/ap2/libs"
os.makedirs(dst, exist_ok=True)
for p in libs:
    shutil.copy2(p, os.path.join(dst, os.path.basename(p)))
print("copied", len(libs), "libs")
PY

COPY airplay.sh /out/airplay.sh
COPY ap2/avahi-daemon.conf /out/ap2/avahi-daemon.conf
COPY ap2/avahi-dbus.conf /out/ap2/avahi-dbus.conf
RUN chmod 755 /out/airplay.sh /out/airplayd /out/ap2/shairport-sync-ap2 /out/ap2/nqptp /out/ap2/avahi-daemon && \
    ls -la /out /out/ap2 | head -40 && du -sh /out /out/ap2/libs

FROM scratch AS out
COPY --from=build /out /
