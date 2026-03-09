#!/usr/bin/env bash
set -euo pipefail

SCRIPT_NAME="$(basename "$0")"

usage() {
  cat <<EOF
Usage:
  $SCRIPT_NAME <version> [options]

Arguments:
  version                  Release version/tag, e.g. v1.2.3

Options:
  --repo OWNER/REPO        GitHub repo for release (default: detected from origin)
  --branch BRANCH          Git branch to push (default: current branch)
  --bin PATH               Firmware bin path (default: auto-detect)
  --public-ota-repo URL    Public OTA repo URL (default: git@github.com:git-beginner-123/OTA.git)
  --public-ota-dir DIR     Local clone dir for public OTA repo (default: /tmp/stem_ota_public_repo)
  --public-ota-branch NAME Public OTA branch to push (default: main)
  --public-ota-subdir DIR  OTA subdir in public repo (default: ota_bins)
  --public-ota-target TAG  Target slug in public repo (default: stem)
  --no-public-sync         Do not sync to public OTA repo
  --no-build               Skip idf.py build
  --notes TEXT             Release notes (default: "OTA release <version>")
  --git-only               Only use git push/tag; skip GitHub Release upload
  -h, --help               Show this help

What it does:
  1) Optional build (idf.py -DPROJECT_VER=<version> build)
  2) Copy firmware to ota/app-<version>.bin
  3) Update ota/latest.bin for stable OTA URL
  4) Generate ota/app-<version>.sha256 and ota/latest.sha256
  5) Commit + push code and ota files
  6) Create git tag and push
  7) Create/update GitHub release and upload bin + sha256 (unless --git-only)
  8) Sync to public OTA repo path ota_bins/<target>/latest.bin (unless --no-public-sync)
EOF
}

die() {
  echo "ERROR: $*" >&2
  exit 1
}

info() {
  echo "[INFO] $*"
}

extract_project_version() {
  local desc="build/project_description.json"
  [ -f "$desc" ] || return 1
  sed -n 's/^[[:space:]]*"project_version":[[:space:]]*"\(.*\)",[[:space:]]*$/\1/p' "$desc" | head -n 1
}

verify_project_version_matches() {
  local expected="$1"
  local actual
  actual="$(extract_project_version || true)"
  [ -n "$actual" ] || die "Cannot read project_version from build/project_description.json. Run build first."
  if [ "$actual" != "$expected" ]; then
    die "Version mismatch: build project_version='$actual' but expected '$expected'."
  fi
}

ensure_idf_env() {
  if command -v idf.py >/dev/null 2>&1; then
    return 0
  fi
  if [ -n "${IDF_PATH:-}" ] && [ -f "${IDF_PATH}/export.sh" ]; then
    # shellcheck source=/dev/null
    . "${IDF_PATH}/export.sh" >/dev/null 2>&1 || true
  fi
  if command -v idf.py >/dev/null 2>&1; then
    return 0
  fi
  die "idf.py not found. Run: '. \$IDF_PATH/export.sh' first, or re-run with --no-build and --bin."
}

detect_repo_from_origin() {
  local origin
  origin="$(git remote get-url origin 2>/dev/null || true)"
  [ -n "$origin" ] || return 1

  if [[ "$origin" =~ ^git@github\.com:([^/]+/[^/]+)(\.git)?$ ]]; then
    echo "${BASH_REMATCH[1]%.git}"
    return 0
  fi
  if [[ "$origin" =~ ^https://github\.com/([^/]+/[^/]+)(\.git)?$ ]]; then
    echo "${BASH_REMATCH[1]%.git}"
    return 0
  fi
  return 1
}

detect_bin_path() {
  local project_name default_bin
  project_name="$(basename "$(pwd)")"
  default_bin="build/${project_name}.bin"
  if [ -f "$default_bin" ]; then
    echo "$default_bin"
    return 0
  fi

  local candidates
  candidates="$(find build -maxdepth 1 -type f -name "*.bin" ! -name "bootloader.bin" ! -name "partition-table.bin" ! -name "ota_data_initial.bin" 2>/dev/null || true)"
  local count
  count="$(echo "$candidates" | sed '/^$/d' | wc -l)"
  if [ "$count" -eq 1 ]; then
    echo "$candidates"
    return 0
  fi
  return 1
}

sync_to_public_ota_repo() {
  local version="$1"
  local source_repo="$2"
  local source_branch="$3"
  local repo_url="$4"
  local repo_dir="$5"
  local public_branch="$6"
  local subdir="$7"
  local target="$8"
  local ota_bin="$9"
  local ota_sha="${10}"
  local ota_latest="${11}"
  local ota_latest_sha="${12}"

  command -v git >/dev/null 2>&1 || {
    info "git not found, skip public OTA sync"
    return 0
  }

  if [[ ! -d "${repo_dir}/.git" ]]; then
    rm -rf "${repo_dir}"
    git clone "${repo_url}" "${repo_dir}"
  else
    git -C "${repo_dir}" remote set-url origin "${repo_url}"
  fi

  local branch="${public_branch:-main}"

  git -C "${repo_dir}" fetch origin --prune || true
  if git -C "${repo_dir}" show-ref --verify --quiet "refs/remotes/origin/${branch}"; then
    git -C "${repo_dir}" checkout -B "${branch}" "origin/${branch}"
  else
    git -C "${repo_dir}" checkout -B "${branch}"
  fi

  local dst_dir="${repo_dir}/${subdir}/${target}"
  mkdir -p "${dst_dir}"
  cp -f "${ota_latest}" "${dst_dir}/latest.bin"
  cp -f "${ota_latest_sha}" "${dst_dir}/latest.sha256"
  cp -f "${ota_bin}" "${dst_dir}/app-${version}.bin"
  cp -f "${ota_sha}" "${dst_dir}/app-${version}.sha256"
  printf '%s\n' "${version}" > "${dst_dir}/latest.version"
  cat > "${dst_dir}/latest.meta" <<EOF
project=stem_framework_idf61_lcd
target=${target}
version=${version}
source_repo=${source_repo}
source_branch=${source_branch}
EOF

  mkdir -p "${repo_dir}/${subdir}"
  local targets_file="${repo_dir}/${subdir}/targets.txt"
  if [[ ! -f "${targets_file}" ]]; then
    : > "${targets_file}"
  fi
  if ! grep -Eq '(^|[[:space:]])STEM[[:space:]]+stem$' "${targets_file}"; then
    printf '%s\n' "9 STEM stem" >> "${targets_file}"
  fi

  git -C "${repo_dir}" add "${subdir}/${target}" "${subdir}/targets.txt"
  if git -C "${repo_dir}" diff --cached --quiet; then
    info "Public OTA repo unchanged (${subdir}/${target})"
    return 0
  fi

  git -C "${repo_dir}" commit -m "ota: update ${target} (${version})"
  git -C "${repo_dir}" push origin "${branch}"
  info "Public OTA synced: ${repo_url} -> ${subdir}/${target}"
}

main() {
  [ $# -ge 1 ] || {
    usage
    exit 1
  }

  local version="$1"
  shift
  if [[ "$version" == "-h" || "$version" == "--help" ]]; then
    usage
    exit 0
  fi

  local repo=""
  local branch=""
  local bin_path=""
  local public_sync=1
  local public_repo_url="${PUBLIC_OTA_REPO_URL:-git@github.com:git-beginner-123/OTA.git}"
  local public_repo_dir="${PUBLIC_OTA_REPO_DIR:-/tmp/stem_ota_public_repo}"
  local public_branch="${PUBLIC_OTA_BRANCH:-main}"
  local public_subdir="${PUBLIC_OTA_SUBDIR:-ota_bins}"
  local public_target="${PUBLIC_OTA_TARGET:-stem}"
  local do_build=1
  local notes=""
  local git_only=0

  while [ $# -gt 0 ]; do
    case "$1" in
      --repo)
        repo="$2"
        shift 2
        ;;
      --branch)
        branch="$2"
        shift 2
        ;;
      --bin)
        bin_path="$2"
        shift 2
        ;;
      --public-ota-repo)
        public_repo_url="$2"
        shift 2
        ;;
      --public-ota-dir)
        public_repo_dir="$2"
        shift 2
        ;;
      --public-ota-branch)
        public_branch="$2"
        shift 2
        ;;
      --public-ota-subdir)
        public_subdir="$2"
        shift 2
        ;;
      --public-ota-target)
        public_target="$2"
        shift 2
        ;;
      --no-public-sync)
        public_sync=0
        shift
        ;;
      --no-build)
        do_build=0
        shift
        ;;
      --notes)
        notes="$2"
        shift 2
        ;;
      --git-only)
        git_only=1
        shift
        ;;
      -h|--help)
        usage
        exit 0
        ;;
      *)
        die "Unknown option: $1"
        ;;
    esac
  done

  git rev-parse --is-inside-work-tree >/dev/null 2>&1 || die "Not a git repository."

  if [ -z "$branch" ]; then
    branch="$(git branch --show-current)"
  fi
  [ -n "$branch" ] || die "Cannot detect current branch. Use --branch."

  if [ -z "$repo" ]; then
    repo="$(detect_repo_from_origin || true)"
  fi
  [ -n "$repo" ] || die "Cannot detect GitHub repo from origin. Use --repo OWNER/REPO."

  if [ -z "$notes" ]; then
    notes="OTA release ${version}"
  fi

  if [ "$do_build" -eq 1 ]; then
    ensure_idf_env
    info "Building firmware with forced project version: ${version}"
    idf.py -DPROJECT_VER="${version}" build
    verify_project_version_matches "${version}"
  else
    info "Skipping build (--no-build)"
    verify_project_version_matches "${version}"
  fi

  if [ -z "$bin_path" ]; then
    bin_path="$(detect_bin_path || true)"
  fi
  [ -n "$bin_path" ] || die "Cannot auto-detect firmware bin. Use --bin PATH."
  [ -f "$bin_path" ] || die "Firmware bin not found: $bin_path"

  local ota_dir ota_bin ota_sha ota_latest ota_latest_sha
  ota_dir="ota"
  ota_bin="${ota_dir}/app-${version}.bin"
  ota_sha="${ota_dir}/app-${version}.sha256"
  ota_latest="${ota_dir}/latest.bin"
  ota_latest_sha="${ota_dir}/latest.sha256"
  mkdir -p "$ota_dir"

  info "Preparing OTA artifacts"
  cp -f "$bin_path" "$ota_bin"
  cp -f "$bin_path" "$ota_latest"
  sha256sum "$ota_bin" >"$ota_sha"
  sha256sum "$ota_latest" >"$ota_latest_sha"
  local src_hash ota_hash latest_hash
  src_hash="$(sha256sum "$bin_path" | awk '{print $1}')"
  ota_hash="$(sha256sum "$ota_bin" | awk '{print $1}')"
  latest_hash="$(sha256sum "$ota_latest" | awk '{print $1}')"
  [ "$src_hash" = "$ota_hash" ] || die "Hash mismatch: source bin and ota/app-${version}.bin differ."
  [ "$src_hash" = "$latest_hash" ] || die "Hash mismatch: source bin and ota/latest.bin differ."

  info "Staging git changes (excluding build outputs)"
  git add . ':!build/' ':!idf_snapshots/'
  git add "$ota_bin" "$ota_sha" "$ota_latest" "$ota_latest_sha"

  if ! git diff --cached --quiet; then
    info "Committing changes"
    git commit -m "release: ${version}"
  else
    info "No staged changes to commit"
  fi

  info "Pushing branch: ${branch}"
  git push origin "$branch"

  if git rev-parse -q --verify "refs/tags/${version}" >/dev/null; then
    info "Tag already exists locally: ${version}"
  else
    info "Creating tag: ${version}"
    git tag "${version}"
  fi

  if git ls-remote --tags origin "refs/tags/${version}" | rg -q .; then
    info "Tag already exists on origin: ${version}"
  else
    info "Pushing tag: ${version}"
    git push origin "${version}"
  fi

  if [ "$public_sync" -eq 1 ]; then
    info "Syncing to public OTA repo (${public_target})"
    sync_to_public_ota_repo \
      "${version}" "${repo}" "${branch}" \
      "${public_repo_url}" "${public_repo_dir}" "${public_branch}" "${public_subdir}" "${public_target}" \
      "${ota_bin}" "${ota_sha}" "${ota_latest}" "${ota_latest_sha}"
  else
    info "Skip public OTA sync (--no-public-sync)"
  fi

  if [ "$git_only" -eq 1 ]; then
    info "Skipping GitHub Release upload (--git-only)"
    info "Stable OTA URL (recommended):"
    echo "https://raw.githubusercontent.com/${repo}/${branch}/ota/latest.bin"
    info "Public OTA URL (shared repo):"
    echo "https://raw.githubusercontent.com/git-beginner-123/OTA/${public_branch}/${public_subdir}/${public_target}/latest.bin"
    info "Version-pinned OTA URL:"
    echo "https://raw.githubusercontent.com/${repo}/${version}/ota/$(basename "$ota_bin")"
    exit 0
  fi

  command -v gh >/dev/null 2>&1 || die "GitHub CLI 'gh' not found. Re-run with --git-only or install gh."

  info "Publishing GitHub release assets to ${repo}"
  if gh release view "${version}" --repo "${repo}" >/dev/null 2>&1; then
    gh release upload "${version}" "$ota_bin" "$ota_sha" --repo "${repo}" --clobber
    info "Release exists; assets uploaded with --clobber"
  else
    gh release create "${version}" "$ota_bin" "$ota_sha" \
      --repo "${repo}" \
      --title "${version}" \
      --notes "${notes}"
    info "Release created"
  fi

  info "Done"
  info "OTA BIN URL:"
  echo "https://github.com/${repo}/releases/download/${version}/$(basename "$ota_bin")"
}

main "$@"
