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

.PHONY: all run frontend cpp configure setup deps clean rebuild dev dev-hot dev-rebuild dev-wait dev-stop \
        run-tests bridge_test frontend-tests perf-bench test-all help

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

## 开发模式（推荐）：Vite 热重载 + Qt 加载开发服务器
# 前端改动即时生效（HMR），无需重编 C++ 或重新打包。
# 用法：make dev          —— 启动 Vite dev server + 应用（Ctrl-C 一并退出）
#      make dev-hot      —— 只起 Vite，另开终端手动运行应用
#      make dev-rebuild  —— 旧的「全量重建后运行」（不依赖 dev server）
#
# QWebChannel 的 transport 由 QWebEnginePage::setWebChannel 注入，与页面
# scheme 无关，因此 http://localhost 下 bridge 同样可用。
DEV_PORT ?= 5173
DEV_URL  ?= http://localhost:$(DEV_PORT)

dev: cpp
	@echo "==> [dev] 启动 Vite dev server（$(DEV_URL)）与 Qt 应用"
	@echo "==> [dev] 前端改动即时生效；Ctrl-C 退出"
	@# 预检端口：Vite 配了 strictPort，若被旧实例占用会直接退出，
	@# 但 dev-wait 探测到的是旧实例的 /@vite/client，应用会静默连到过期页面。
	@if lsof -nP -tiTCP:$(DEV_PORT) -sTCP:LISTEN >/dev/null 2>&1; then \
		echo "==> [dev] 端口 $(DEV_PORT) 已被占用（可能是上次残留的 Vite）；请先执行 make dev-stop"; \
		exit 1; \
	fi
	@# 用子 shell + exec 启动，保证 $$! 就是 dev server 进程本身，
	@# 而不是「cd && npm」这条后台链的壳进程（否则 Ctrl-C 只杀壳、Vite 变孤儿）。
	@# 清理时同时按监听端口兜底，避免 npm → node/vite 的子进程残留。
	@( cd $(ROOT)/frontend && exec $(NPM) run dev -- --port $(DEV_PORT) ) > $(BUILD_DIR)/vite-dev.log 2>&1 & \
		VITE_PID=$$!; \
		cleanup() { \
			PIDS=$$(lsof -nP -tiTCP:$(DEV_PORT) -sTCP:LISTEN 2>/dev/null); \
			if [ -n "$$PIDS" ]; then kill $$PIDS 2>/dev/null || true; fi; \
			kill $$VITE_PID 2>/dev/null || true; \
		}; \
		trap 'cleanup; exit 129' HUP; \
		trap 'cleanup; exit 130' INT TERM; \
		trap cleanup EXIT; \
		$(MAKE) --no-print-directory dev-wait; \
		QTRS_DEV_URL=$(DEV_URL) $(BIN)

# 等待 Vite dev server 就绪（内部目标，由 make dev 调用，不在 help 中展示）
# 注意：不能用 GET / 判定就绪——Vite 端口一旦 listen 就会返回 200，
# 但此时模块转换尚未就绪，Qt 侧加载会失败。这里探测真实模块端点。
dev-wait:
	@for i in $$(seq 1 60); do \
		if curl -fsS --noproxy '*' --max-time 2 -o /dev/null $(DEV_URL)/@vite/client 2>/dev/null; then \
			echo "==> [dev] Vite dev server 已就绪"; exit 0; \
		fi; \
		sleep 0.5; \
	done; \
	echo "==> [dev] 警告：等待 Vite dev server 超时（继续尝试启动应用）"; \
	echo "          若 $(DEV_PORT) 端口被占用，请先执行 make dev-stop"

## 清理可能残留的 Vite dev server（Ctrl-C 未正常退出时）
dev-stop:
	@PIDS=$$(lsof -nP -tiTCP:$(DEV_PORT) -sTCP:LISTEN 2>/dev/null); \
	if [ -n "$$PIDS" ]; then \
		echo "==> [dev-stop] 结束占用 $(DEV_PORT) 的进程: $$PIDS"; \
		kill $$PIDS 2>/dev/null || true; \
	else \
		echo "==> [dev-stop] $(DEV_PORT) 端口空闲"; \
	fi

## 开发模式（仅前端热重载，不自动启动应用）
dev-hot: cpp
	@echo "==> [dev-hot] 仅启动 Vite dev server（$(DEV_URL)）"
	@echo "==> [dev-hot] 另开终端运行：QTRS_DEV_URL=$(DEV_URL) $(BIN)"
	@cd $(ROOT)/frontend && $(NPM) run dev -- --port $(DEV_PORT)

## 旧的「全量重建后运行」：清空 build 与前端产物再重建
dev-rebuild: clean all
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
