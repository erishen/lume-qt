# ============================================================================
# qt-react-shell — Makefile
#
# 常用目标:
#   make             完整构建（前端 + C++）
#   make run         构建并运行应用
#   make frontend    仅构建前端（Vite 产物 → resources/web）
#   make cpp         仅配置 + 编译 C++（CMake 会自动同步前端）
#   make clean       清理 build 与前端产物
#   make setup       安装前端依赖
#
# 环境变量可覆盖: NPM / CMAKE / QT_PREFIX
# ============================================================================

ROOT      := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BUILD_DIR := $(ROOT)/build
BIN       := $(BUILD_DIR)/qt-react-shell
WEB_ROOT  := $(ROOT)/resources/web

QT_PREFIX ?= /opt/homebrew/opt/qt
# Homebrew 根前缀：用于让 find_package 在 brew install 后自动发现
# googletest / dcmtk 等装在 /opt/homebrew 下的包（Qt 本身走 QT_PREFIX）。
HOMEBREW_PREFIX ?= /opt/homebrew
PREFIX_PATH ?= $(QT_PREFIX):$(HOMEBREW_PREFIX)
NPM        ?= npm
CMAKE      ?= cmake
GENERATOR  ?= Ninja

.PHONY: all run frontend cpp configure setup deps clean rebuild dev run-tests \
        bridge_test frontend-tests perf-bench test-all help

# ctest 可执行文件（与 cmake 同目录）
CTEST ?= ctest

# 默认目标：完整构建
all: cpp

## 构建并运行应用
run: all
	@echo "==> 启动 $(BIN)"
	$(BIN)

## 仅安装前端依赖（幂等）
setup: deps

deps:
	@if [ ! -d "$(ROOT)/frontend/node_modules" ]; then \
		echo "==> [setup] npm install"; \
		cd $(ROOT)/frontend && $(NPM) install; \
	else \
		echo "==> [setup] node_modules 已存在，跳过 npm install"; \
	fi

## 仅构建前端（预览用，Vite 产物 → resources/web）
frontend: deps
	@echo "==> [frontend] npm run build"
	cd $(ROOT)/frontend && $(NPM) run build

## 配置 CMake（仅在需要时重新生成）
configure:
	@echo "==> [cmake] configure -G $(GENERATOR)"
	$(CMAKE) -S $(ROOT) -B $(BUILD_DIR) \
		-DCMAKE_PREFIX_PATH=$(PREFIX_PATH) \
		-G $(GENERATOR)

## 编译 C++（CMake 的 frontend-build 目标会自动同步前端）
cpp: configure deps
	@echo "==> [cpp] cmake --build"
	$(CMAKE) --build $(BUILD_DIR)

## 清理构建产物
clean:
	@echo "==> [clean] 移除 $(BUILD_DIR) 与 $(WEB_ROOT)"
	rm -rf $(BUILD_DIR)
	rm -rf $(WEB_ROOT)
	mkdir -p $(WEB_ROOT)
	touch $(WEB_ROOT)/.gitkeep

## 清理后完整重建
rebuild: clean all

## 开发构建（带调试支持）
# 重新构建并运行，适用于开发和调试
dev: clean all
	@echo "==> 开发构建：重新构建完整项目"
	$(MAKE) all
	@echo "==> 启动应用（开发模式）"
	$(BIN)

## 运行 C++ 单元测试（Google Test，经 ctest）
run-tests: cpp
	@echo "==> 运行 C++ 单元测试 (ctest)"
	@cd $(BUILD_DIR) && $(CTEST) --output-on-failure || \
		(echo "  未找到测试。请确认已 brew install googletest 并重新 configure。" && exit 1)

## 仅构建并运行 C++ 单测可执行（bridge_test）
bridge_test: run-tests
	@true

## 运行前端单元测试（npm test）
## 注意：前端 Jest/RTL 测试尚未接入（见 README “测试基础设施”），
## 目前 package.json 无 test 脚本，本目标会明确提示而非静默失败。
frontend-tests: deps
	@echo "==> 前端单元测试"
	@cd $(ROOT)/frontend && \
		if node -e "process.exit(require('./package.json').scripts && require('./package.json').scripts.test ? 0 : 1)" 2>/dev/null; then \
			$(NPM) test; \
		else \
			echo "  [跳过] 前端单测未实现：package.json 暂无 test 脚本 / Jest 依赖。"; \
			echo "  参见 README “测试基础设施” 中的规划说明。"; \
		fi

## 运行性能基准（CMake 目标 perf-benchmark）
perf-bench: cpp
	@echo "==> 性能基准"
	@$(CMAKE) --build $(BUILD_DIR) --target perf-benchmark

## 运行完整测试套件（C++ + 前端）
test-all: run-tests frontend-tests

## 帮助
help:
	@echo "qt-react-shell 可用目标:"
	@awk '/^## / { desc = substr($$0, 4); next } \
	      /^[a-zA-Z_][a-zA-Z0-9_-]*:/ { if (desc != "") { \
	          target = $$0; sub(/:.*/, "", target); \
	          printf "  make %-11s %s\n", target, desc; desc = "" } }' \
	      $(MAKEFILE_LIST)
