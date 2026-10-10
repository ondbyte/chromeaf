# chromeaf — ungoogled-chromium with a built-in MCP control server.
#
# Quick start (Linux):
#   make deps          # submodules + host tool check
#   make fetch         # Chromium source tarball for the pinned version
#   make prepare       # prune, patch (ungoogled + portablelinux + chromeaf),
#                      # domain substitution, toolchain
#   make linux         # gn gen + ninja chrome chromeaf_unittests
#   make run           # launch with the MCP server on $(MCP_PORT)
#   make smoke         # end-to-end test of every tool against a fresh instance
#
# Everything is plain Chromium underneath: you can also run gn/ninja directly
# in $(SRC) with the args.gn written to $(OUT).

SHELL := /bin/bash
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

BUILD_DIR ?= $(ROOT)/build
SRC ?= $(BUILD_DIR)/src
OUT ?= $(SRC)/out/chromeaf
# tarball (default, what portablelinux releases use) or clone (git, slower).
FETCH ?= tarball
# release (official, LTO) or dev (component build, DCHECKs on).
BUILD_TYPE ?= release
TARGET_CPU ?=
JOBS ?= $(shell nproc)
TARGETS ?= chrome chromeaf_unittests

MCP_PORT ?= 9223
MCP_HOST ?= 127.0.0.1
MCP_TOKEN ?=
PROFILE ?= $(BUILD_DIR)/profile
RUN_ARGS ?=

ANDROID_CPU ?= arm64
ANDROID_UC_PATCHES ?= 0
ANDROID_ROOT ?= $(BUILD_DIR)/android
ANDROID_OUT ?= $(ANDROID_ROOT)/src/out/chromeaf-android

DOCKER_IMAGE ?= chromeaf-build

export BUILD_DIR SRC OUT FETCH BUILD_TYPE TARGET_CPU JOBS TARGETS
export ANDROID_CPU ANDROID_UC_PATCHES ANDROID_ROOT ANDROID_OUT

STEP := $(ROOT)/scripts/chromeaf.sh

.DEFAULT_GOAL := help
.PHONY: help deps check-deps fetch prepare overlay gn linux build test \
        unittests smoke run package clean distclean \
        docker-image docker-linux \
        android-fetch android-prepare android-gn android android-install \
        android-run

help: ## Show this help.
	@awk 'BEGIN {FS = ":.*## "} /^[a-zA-Z_-]+:.*## / \
	  {printf "  \033[36m%-16s\033[0m %s\n", $$1, $$2}' $(MAKEFILE_LIST)
	@echo
	@echo "  Variables: FETCH=$(FETCH) BUILD_TYPE=$(BUILD_TYPE) JOBS=$(JOBS)"
	@echo "             OUT=$(OUT) MCP_PORT=$(MCP_PORT)"

# --- Linux ------------------------------------------------------------------

deps: ## Init submodules and check host build tools.
	@$(STEP) submodules
	@$(STEP) check-deps

check-deps: ## Only check host build tools.
	@$(STEP) check-deps

fetch: ## Download/unpack (or clone) the pinned Chromium source.
	@$(STEP) fetch

prepare: ## Prune, apply all patches, domain-substitute, set up toolchain.
	@$(STEP) prepare

overlay: ## Re-copy chromeaf/ sources into the tree (after editing them).
	@$(STEP) overlay

gn: ## Write args.gn and run gn gen.
	@$(STEP) gn

linux: ## Build chrome and chromeaf_unittests (runs gn if needed).
	@$(STEP) build

build: linux

unittests: ## Run chromeaf_unittests.
	@$(STEP) unittests

test: unittests smoke ## Unit tests + end-to-end smoke test.

smoke: ## Launch a fresh instance and exercise every MCP tool.
	python3 $(ROOT)/scripts/mcp_smoke_test.py --launch $(OUT)/chrome \
	  --url http://127.0.0.1:$(MCP_PORT)/mcp \
	  $(if $(MCP_TOKEN),--token $(MCP_TOKEN))

run: ## Launch chromeaf with the MCP server enabled.
	$(OUT)/chrome --user-data-dir=$(PROFILE) \
	  --chromeaf-mcp-port=$(MCP_PORT) --chromeaf-mcp-host=$(MCP_HOST) \
	  $(if $(MCP_TOKEN),--chromeaf-mcp-token=$(MCP_TOKEN)) $(RUN_ARGS)

package: ## Create build/release/chromeaf-<version>-<cpu>-linux.tar.xz.
	@$(STEP) package

clean: ## Remove the build output directory ($(OUT)).
	rm -rf $(OUT)

distclean: ## Remove everything under build/ (sources, caches, outputs).
	rm -rf $(BUILD_DIR)/src $(BUILD_DIR)/android $(BUILD_DIR)/download_cache \
	  $(BUILD_DIR)/depot_tools $(BUILD_DIR)/release $(BUILD_DIR)/profile \
	  $(BUILD_DIR)/domsubcache.tar.gz

# --- Containerised Linux build ------------------------------------------------

docker-image: ## Build the Debian build image (from portablelinux).
	docker build -t $(DOCKER_IMAGE) \
	  -f $(ROOT)/platform/portablelinux/docker/build.Dockerfile \
	  $(ROOT)/platform/portablelinux/docker

docker-linux: docker-image ## fetch+prepare+build inside the build image.
	docker run --rm -it -v $(ROOT):/repo -w /repo $(DOCKER_IMAGE) \
	  make fetch prepare linux FETCH=$(FETCH) BUILD_TYPE=$(BUILD_TYPE) \
	  JOBS=$(JOBS)

# --- Android (experimental) -------------------------------------------------

android-fetch: ## depot_tools checkout of the pinned version for Android.
	@$(STEP) android-fetch

android-prepare: ## Apply chromeaf (and optionally ungoogled) patches.
	@$(STEP) android-prepare

android-gn: ## gn gen for Android ($(ANDROID_CPU)).
	@$(STEP) android-gn

android: ## Build chrome_public_apk.
	@$(STEP) android-build

android-install: ## adb install the APK and forward the MCP port.
	adb install -r $(ANDROID_OUT)/apks/ChromePublic.apk
	adb shell "echo '_ --chromeaf-mcp-port=$(MCP_PORT)' > /data/local/tmp/chrome-command-line"
	adb forward tcp:$(MCP_PORT) tcp:$(MCP_PORT)
	@echo "Start the app; MCP is then at http://127.0.0.1:$(MCP_PORT)/mcp"

android-run: ## Start the installed app.
	adb shell am start -n org.chromium.chrome/com.google.android.apps.chrome.Main
