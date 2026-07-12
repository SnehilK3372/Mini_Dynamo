#!/usr/bin/env bash
#
# One-time bootstrap for a fresh EC2 instance (Amazon Linux 2023 or Ubuntu).
# Installs Docker + the Compose plugin, clones the repo, and brings the stack up.
# Idempotent — safe to re-run. Can be pasted into the instance's "user data" at
# launch, or run manually after SSH-ing in:
#
#   curl -fsSL https://raw.githubusercontent.com/SnehilK3372/Mini_Dynamo/main/deploy/aws/bootstrap.sh | bash
#   # ...or clone first and run ./deploy/aws/bootstrap.sh
#
# After it runs, set real secrets in the repo's .env (see deploy/aws/.env.example)
# and re-run `docker compose up -d`.
set -euo pipefail

REPO_URL="${REPO_URL:-https://github.com/SnehilK3372/Mini_Dynamo.git}"
REPO_DIR="${REPO_DIR:-$HOME/Mini_Dynamo}"
BRANCH="${BRANCH:-main}"

log() { printf '\n=== %s ===\n' "$*"; }

# --- Docker + Compose plugin --------------------------------------------------
install_docker() {
  if command -v docker >/dev/null 2>&1; then
    log "Docker already installed ($(docker --version))"
    return
  fi
  log "Installing Docker + Compose plugin"
  if [ -f /etc/os-release ]; then . /etc/os-release; fi
  case "${ID:-}" in
    amzn)  # Amazon Linux 2023
      sudo dnf -y install docker
      sudo systemctl enable --now docker
      # Compose plugin (Amazon Linux packages it separately or as a plugin binary).
      sudo mkdir -p /usr/libexec/docker/cli-plugins
      sudo curl -fsSL "https://github.com/docker/compose/releases/latest/download/docker-compose-$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)" \
        -o /usr/libexec/docker/cli-plugins/docker-compose
      sudo chmod +x /usr/libexec/docker/cli-plugins/docker-compose
      ;;
    ubuntu|debian)
      sudo apt-get update
      sudo apt-get install -y ca-certificates curl gnupg
      sudo install -m 0755 -d /etc/apt/keyrings
      curl -fsSL "https://download.docker.com/linux/${ID}/gpg" | sudo gpg --dearmor -o /etc/apt/keyrings/docker.gpg
      sudo chmod a+r /etc/apt/keyrings/docker.gpg
      echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] https://download.docker.com/linux/${ID} $(. /etc/os-release && echo "$VERSION_CODENAME") stable" \
        | sudo tee /etc/apt/sources.list.d/docker.list >/dev/null
      sudo apt-get update
      sudo apt-get install -y docker-ce docker-ce-cli containerd.io docker-compose-plugin
      sudo systemctl enable --now docker
      ;;
    *)
      echo "Unsupported OS '${ID:-unknown}'. Install Docker + the compose plugin manually." >&2
      exit 1
      ;;
  esac
  # Let the login user run docker without sudo (effective on next login).
  sudo usermod -aG docker "$USER" || true
}

# --- Repo + stack -------------------------------------------------------------
fetch_repo() {
  if [ -d "$REPO_DIR/.git" ]; then
    log "Updating repo at $REPO_DIR"
    git -C "$REPO_DIR" fetch origin "$BRANCH"
    git -C "$REPO_DIR" checkout "$BRANCH"
    git -C "$REPO_DIR" pull --ff-only origin "$BRANCH"
  else
    log "Cloning $REPO_URL -> $REPO_DIR"
    git -C "$(dirname "$REPO_DIR")" clone --branch "$BRANCH" "$REPO_URL" "$(basename "$REPO_DIR")"
  fi
}

seed_env() {
  if [ ! -f "$REPO_DIR/.env" ]; then
    log "Creating .env from template (EDIT IT — set real secrets before exposing publicly)"
    cp "$REPO_DIR/deploy/aws/.env.example" "$REPO_DIR/.env"
  else
    log ".env already exists — leaving it untouched"
  fi
}

bring_up() {
  log "Starting the stack (docker compose up -d --build)"
  # `sg docker` runs with the freshly-added docker group without needing re-login.
  cd "$REPO_DIR"
  if docker info >/dev/null 2>&1; then
    docker compose up -d --build
  else
    sudo docker compose up -d --build
  fi
  log "Done. Gateway on :8080. Set secrets in $REPO_DIR/.env then re-run 'docker compose up -d'."
}

command -v git >/dev/null 2>&1 || { sudo dnf -y install git 2>/dev/null || sudo apt-get install -y git; }
install_docker
fetch_repo
seed_env
bring_up
