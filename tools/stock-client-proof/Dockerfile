FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55

ENV DEBIAN_FRONTEND=noninteractive
RUN --mount=type=secret,id=host_ca,required=true,mode=0444 \
    sed -i 's|http://|https://|g' /etc/apt/sources.list.d/ubuntu.sources \
    && apt-get -o Acquire::https::CaInfo=/run/secrets/host_ca -o APT::Update::Error-Mode=any update \
    && apt-get -o Acquire::https::CaInfo=/run/secrets/host_ca install -y --no-install-recommends \
    ca-certificates python3 openjdk-21-jdk-headless xvfb xauth \
    libgl1 libegl1 libopengl0 libx11-xcb1 libdbus-1-3 libfontconfig1 libharfbuzz0b libxkbcommon-x11-0 \
    libxcb-cursor0 libxcb-icccm4 libxcb-keysyms1 libxcb-image0 \
    libxcb-render-util0 libxcb-xinerama0 libxcb-xinput0 \
    && rm -rf /var/lib/apt/lists/*

ENV LANG=C.UTF-8
USER 1000:1000
CMD ["sleep", "infinity"]
