# Single toolchain image used by the devcontainer, the CI and the release workflow.
# Base: official Espressif image with ESP-IDF pre-installed (tools, python venv, xtensa/riscv toolchains).
# Bump ESP-IDF here and only here.
ARG IDF_VERSION=v6.0.2
FROM espressif/idf:${IDF_VERSION}

ARG NODE_MAJOR=22
ARG USERNAME=dev
ARG USER_UID=1000
ARG USER_GID=1000

ENV DEBIAN_FRONTEND=noninteractive

# Extra tooling: Node for the web UI, linters, serial helpers, misc.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl gnupg git openssh-client sudo \
        clang-format cppcheck \
        picocom minicom usbutils \
        jq less vim-tiny \
    && curl -fsSL https://deb.nodesource.com/setup_${NODE_MAJOR}.x | bash - \
    && apt-get install -y --no-install-recommends nodejs \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

# Non-root user with access to serial devices (dialout) and passwordless sudo.
RUN groupadd --gid ${USER_GID} ${USERNAME} \
    && useradd --uid ${USER_UID} --gid ${USER_GID} -m -s /bin/bash ${USERNAME} \
    && usermod -aG dialout,plugdev ${USERNAME} 2>/dev/null || usermod -aG dialout ${USERNAME} \
    && echo "${USERNAME} ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/${USERNAME} \
    && chmod 0440 /etc/sudoers.d/${USERNAME}

# The Espressif image installs IDF tools under /opt/esp for root; make them readable by the dev user
# and source the IDF environment in every shell.
RUN chmod -R a+rX /opt/esp \
    && echo '. /opt/esp/idf/export.sh >/dev/null 2>&1 || true' >> /etc/bash.bashrc

# Optional: Claude Code CLI inside the container (comment out if you run it on the host).
RUN npm install -g @anthropic-ai/claude-code

USER ${USERNAME}
WORKDIR /workspaces/esp_dali_gw

# Useful defaults
ENV IDF_TARGET=esp32c6 \
    ESPPORT=/dev/ttyACM0 \
    IDF_CCACHE_ENABLE=1