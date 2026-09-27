.PHONY: init build up up-desktop down ps logs demo check core-test package clean

COMPOSE = docker compose --env-file deploy/.env -f deploy/compose.yaml

init:
	./deploy/scripts/initialize-minicloud.sh

build:
	$(COMPOSE) build

up:
	$(COMPOSE) up -d --build

up-desktop:
	$(COMPOSE) -f deploy/compose.desktop.yaml up -d --build

down:
	./scripts/stop.sh

ps:
	$(COMPOSE) ps

logs:
	$(COMPOSE) logs -f --tail=200

demo:
	./scripts/e2e.sh

check:
	./scripts/check.sh

core-test:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion \
		-Icpp/include cpp/core/domain.cpp cpp/core/scheduler.cpp \
		cpp/core/reconciler.cpp tests/cpp/core_tests.cpp -o /tmp/minicloud-core-tests
	/tmp/minicloud-core-tests

package:
	./scripts/package-release.sh

clean:
	cmake -E remove_directory build
