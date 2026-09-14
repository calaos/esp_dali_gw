# Single toolchain image used by the devcontainer, the CI and the release workflow.
# Base: official Espressif image with ESP-IDF pre-installed (tools, python venv, riscv toolchain).
# Bump ESP-IDF here and only here.
ARG IDF_VERSION=v6.0.2
FROM espressif/idf:${IDF_VERSION}

ARG NODE_MAJOR=22
ARG USERNAME=dev
ARG USER_UID=1000
ARG USER_GID=1000
# The devcontainer passes 1; CI passes 0 so the image stays lean and reproducible.
ARG INSTALL_CLAUDE_CODE=0

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

# GitHub Actions `container:` jobs bypass the image ENTRYPOINT (they docker-exec each step), so the
# IDF environment must be reachable without it. Snapshot what export.sh sets into a plain env file
# and pull it in through BASH_ENV (read by NON-interactive bash, which is what CI steps use) and
# /etc/bash.bashrc (read by interactive shells, which is what a devcontainer terminal uses).
SHELL ["/bin/bash", "-c"]
RUN . /opt/esp/idf/export.sh >/dev/null 2>&1 \
    && env | grep -E '^(PATH|IDF_PATH|IDF_TOOLS_PATH|IDF_PYTHON_ENV_PATH|OPENOCD_SCRIPTS|ESP_ROM_ELF_DIR|ESP_IDF_VERSION)=' \
       | sed 's/^/export /' > /etc/esp-idf-env.sh \
    && chmod 0644 /etc/esp-idf-env.sh \
    && echo '. /etc/esp-idf-env.sh' >> /etc/bash.bashrc \
    && grep -q '^export PATH=' /etc/esp-idf-env.sh \
    && rm -rf /tmp/esp_idf_activate_*
# BASH_ENV is honoured by non-interactive bash only. Every entry point into this image (CI steps,
# tools/ci-local.sh, the devcontainer terminal) uses bash; plain `sh -c` will not see the toolchain.
ENV BASH_ENV=/etc/esp-idf-env.sh
# The base image ENTRYPOINT re-runs export.sh on every start, which is now redundant and fails for a
# non-root user. Everything it did is baked into the image above.
ENTRYPOINT []
CMD ["/bin/bash"]

# Non-root user with access to serial devices (dialout) and passwordless sudo.
# Ubuntu 24.04 ships a stock "ubuntu" account squatting uid/gid 1000; it has to go before ${USERNAME}
# can take that uid, which must match the host user for the bind-mounted workspace to be writable.
RUN set -eux; \
    if getent passwd ${USER_UID} >/dev/null; then \
        userdel -r "$(getent passwd ${USER_UID} | cut -d: -f1)" 2>/dev/null || true; \
    fi; \
    if getent group ${USER_GID} >/dev/null; then \
        groupdel "$(getent group ${USER_GID} | cut -d: -f1)" 2>/dev/null || true; \
    fi; \
    groupadd --gid ${USER_GID} ${USERNAME}; \
    useradd --uid ${USER_UID} --gid ${USER_GID} -m -s /bin/bash ${USERNAME}; \
    usermod -aG dialout,plugdev ${USERNAME}; \
    echo "${USERNAME} ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/${USERNAME}; \
    chmod 0440 /etc/sudoers.d/${USERNAME}

# The IDF tools live under /opt/esp and are installed for root; make them readable by everyone.
RUN chmod -R a+rX /opt/esp

# Docker creates a missing bind/volume mountpoint as root:root, which breaks `npm ci` and ccache for
# the non-root user. Creating them here with the right owner makes the named volumes inherit it.
RUN mkdir -p /home/${USERNAME}/.cache/ccache /workspaces/esp_dali_gw/web/node_modules \
    && chown -R ${USER_UID}:${USER_GID} /home/${USERNAME}/.cache /workspaces

# The workspace is bind-mounted from a host with a different uid; without this every git call in the
# container fails with "detected dubious ownership".
RUN git config --system --add safe.directory '*'

# Optional: Claude Code CLI inside the container. Off by default so the CI image stays reproducible.
RUN if [ "${INSTALL_CLAUDE_CODE}" = "1" ]; then npm install -g @anthropic-ai/claude-code; fi

USER ${USERNAME}
WORKDIR /workspaces/esp_dali_gw

# IDF_TARGET is deliberately NOT set: it silently overrides `idf.py set-target`, so building for
# another chip appears to work and produces the old target. The target comes from sdkconfig, which
# set-target writes.
ENV ESPPORT=/dev/ttyACM0 \
    CCACHE_DIR=/home/${USERNAME}/.cache/ccache \
    IDF_CCACHE_ENABLE=1
