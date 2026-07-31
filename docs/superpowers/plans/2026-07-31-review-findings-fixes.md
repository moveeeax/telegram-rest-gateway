# Review Findings Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix all 16 findings confirmed by the 2026-07-31 multi-agent review (1 Critical SSRF-guard bypass, 14 Important, 1 Minor), each verified by 3 independent adversarial skeptics before being accepted into this plan.

**Architecture:** No new subsystems. Each task is a targeted, minimal-diff fix inside an existing file (or a small, closely related group of files), plus a regression test proving the specific failure scenario the review described no longer reproduces.

**Tech Stack:** C++20 / Drogon / TDLib (core service), TypeScript / Node 22 (archiver sidecar), Helm charts (deploy manifests). Same toolchain as the rest of the repo — no new dependencies.

## Global Constraints

- Every fix must be traceable to exactly one confirmed finding from the review (listed under each task). Do not bundle unrelated cleanup.
- C++: match existing style exactly — comments in Russian, same patterns as neighboring code (e.g. `TdBridge::drainPending()`'s atomic find+erase pattern, `resolveWith`'s loop-marshalling pattern). Run `clang-format` before committing (pinned version 18.1.8, matching CI).
- C++ tests: this repo does **not** spin up a live `drogon::app().run()` HTTP server anywhere in the test suite (see `tests/route_smoke_test.cpp` lines 20-38 for the documented rationale: CI runs `tgw_unit_tests` under ASan/TSan, and a live server in that process is fragile and unverifiable there). Do not introduce one. Test pure/extracted functions and component-level harnesses (the `BridgeHarness`/`LoopThread` pattern already used in `tests/context_builder_test.cpp` and `tests/webhook_dispatcher_test.cpp`) instead.
- `src/http/message_routes.cpp` is explicitly excluded from `route_smoke_test.cpp`'s route table (documented there as a known follow-up, not something to silently "fix" as part of this plan). Task 3 below fixes two concrete bugs in it without attempting to migrate it to the route-table pattern or add a live-server integration test — that migration is out of scope.
- Never weaken the existing safety invariants while fixing something else: TDLib session single-writer (`replicas: 1` + `Recreate`), `RequestState`/`SendWaitState`/webhook in-flight counters' exactly-once-resolution guarantees, idempotency-cache release-or-store-exactly-once-per-branch discipline.
- All new/changed Helm values need a comment explaining *why*, matching the file's existing commenting density.

---

### Task 1: Webhook SSRF-guard IPv4-mapped-IPv6 bypass (Critical) + guard testability + queue_max=0 boundary test

**Finding IDs:** #1 (Critical — SSRF-guard bypass via `::ffff:x.x.x.x`), #14 (Important — `isPrivateHost`/`parseUrl` have zero test coverage, anonymous-namespace, untestable), #16 (Minor — `TGW_WEBHOOK_QUEUE_MAX=0` boundary untested).

**Files:**
- Modify: `src/webhook/webhook_dispatcher.cpp`
- Modify: `src/webhook/webhook_dispatcher.hpp`
- Modify: `tests/webhook_dispatcher_test.cpp`

**Interfaces:**
- Produces: `tgw::webhook::ParsedUrl` struct, `tgw::webhook::parseUrl(const std::string&) -> ParsedUrl`, `tgw::webhook::isPrivateHost(const std::string&) -> bool` — moved from the `.cpp`'s anonymous namespace into the public `tgw::webhook` namespace (declared in the `.hpp`), same pattern already used for `signBody`/`serializeEvent` ("вынесена наружу ради тестов").

**Root cause:** `isPrivateHost` in `src/webhook/webhook_dispatcher.cpp` (current lines 162-201) does string-prefix checks on the IPv6 literal (`"::1"`, `"::"`, first two chars `fc`/`fd`/`fe`) but never decodes the address. An IPv4-mapped IPv6 literal like `::ffff:127.0.0.1` starts with `::` followed by `ffff:`, which matches none of those prefix checks, falls through to `return false`, and the request is delivered — even though `inet_pton(AF_INET6, "::ffff:127.0.0.1", ...)` (which drogon/trantor uses under the hood for a bracketed host) resolves to a real socket address whose embedded IPv4 is `127.0.0.1`.

- [ ] **Step 1: Move `ParsedUrl`/`parseUrl`/`isPrivateHost` out of the anonymous namespace in `src/webhook/webhook_dispatcher.cpp`**

  In `src/webhook/webhook_dispatcher.cpp`, the anonymous `namespace { ... }` block (starting at current line 84) currently contains `toHexLower`, `InFlightToken`, `ParsedUrl`, `parseUrl`, `isPrivateHost`, and `deliverOnLoop`. Split it: `toHexLower`, `InFlightToken`, `deliverOnLoop` stay anonymous; `ParsedUrl`, `parseUrl`, `isPrivateHost` move to file scope directly under `namespace tgw::webhook { ... }` (i.e. become non-anonymous, matching how `signBody`/`serializeEvent` are already defined outside the anonymous namespace a few lines below). `deliverOnLoop` still calls them unqualified — that still resolves via enclosing-namespace lookup, no call-site changes needed.

  Replace the `isPrivateHost` body with one that actually decodes IPv6 literals via `inet_pton` instead of string-prefix matching, and factors the IPv4-octet-range logic into a small helper reused for both plain IPv4 and IPv4-mapped/-compatible IPv6:

  ```cpp
  // Диапазоны приватного/loopback/metadata IPv4 по первым двум октетам (RFC1918 + loopback +
  // облачная metadata 169.254.169.254). Общий хелпер для голого IPv4 и IPv4-mapped/-compatible
  // IPv6 (::ffff:a.b.c.d / ::a.b.c.d) — оба кодируют один и тот же адрес в последних 4 байтах.
  bool isPrivateIpv4Octets(unsigned int o0, unsigned int o1) {
      if (o0 == 127u || o0 == 10u || o0 == 0u) {
          return true;  // loopback / RFC1918 10/8 / 0.0.0.0
      }
      if (o0 == 192u && o1 == 168u) {
          return true;  // 192.168/16
      }
      if (o0 == 172u && o1 >= 16u && o1 <= 31u) {
          return true;  // 172.16/12
      }
      if (o0 == 169u && o1 == 254u) {
          return true;  // link-local + облачная metadata (169.254.169.254)
      }
      return false;
  }
  ```

  This helper goes in the anonymous namespace (it's an internal detail, not needed by tests directly — tests exercise it indirectly through `isPrivateHost`).

  Then, outside the anonymous namespace, in `namespace tgw::webhook { ... }` directly (same place `signBody` lives):

  ```cpp
  // Разбор URL вебхука на base (scheme://host[:port]) + path-with-query + host (для SSRF).
  // Вынесена наружу ради тестов, как signBody/serializeEvent.
  struct ParsedUrl {
      std::string base;  // http(s)://host[:port]
      std::string path;  // /path?query — как есть (не перекодируем, см. setPathEncode(false))
      std::string host;  // host без порта/скобок — для SSRF-проверки
      bool valid = false;
  };

  ParsedUrl parseUrl(const std::string& url) {
      ParsedUrl p;
      const auto scheme_end = url.find("://");
      if (scheme_end == std::string::npos) {
          return p;  // без схемы не работаем (drogon HttpClient требует base со схемой)
      }
      const std::string scheme = url.substr(0, scheme_end);
      if (scheme != "http" && scheme != "https") {
          return p;
      }
      const std::string rest = url.substr(scheme_end + 3);
      const auto slash = rest.find('/');
      const std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
      if (authority.empty()) {
          return p;
      }
      p.base = scheme + "://" + authority;
      p.path = (slash == std::string::npos) ? "/" : rest.substr(slash);
      // Выделяем host из authority (может быть [ipv6]:port или host:port).
      if (authority.front() == '[') {
          const auto close = authority.find(']');
          p.host =
              (close == std::string::npos) ? authority.substr(1) : authority.substr(1, close - 1);
      } else {
          const auto colon = authority.find(':');
          p.host = (colon == std::string::npos) ? authority : authority.substr(0, colon);
      }
      p.valid = true;
      return p;
  }

  // Грубая, но безопасная-по-умолчанию проверка приватного/loopback-хоста для SSRF-guard.
  // Работает по строке хоста: literal-IP разбираем побайтово (inet_pton, не строковый префикс —
  // строковые правила пропускали IPv4-mapped/-compatible IPv6, см. регресс-тест ниже), доменные
  // имена (кроме localhost) считаем внешними (DNS-rebinding вне охвата — резолвинг у drogon, не
  // у нас). Вынесена наружу ради тестов, как signBody/serializeEvent.
  bool isPrivateHost(const std::string& host) {
      if (host.empty() || host == "localhost") {
          return true;
      }
      if (host.find(':') != std::string::npos) {
          unsigned char buf[16];
          if (inet_pton(AF_INET6, host.c_str(), buf) != 1) {
              return true;  // не распарсили IPv6-литерал — fail-closed, это уже guard, не транспорт
          }
          bool all_zero = true;
          for (int i = 0; i < 16; ++i) {
              if (buf[i] != 0) {
                  all_zero = false;
                  break;
              }
          }
          if (all_zero) {
              return true;  // :: (unspecified)
          }
          bool loopback = true;
          for (int i = 0; i < 15; ++i) {
              if (buf[i] != 0) {
                  loopback = false;
                  break;
              }
          }
          if (loopback && buf[15] == 1) {
              return true;  // ::1
          }
          if ((buf[0] & 0xFEu) == 0xFCu) {
              return true;  // fc00::/7 unique-local
          }
          if (buf[0] == 0xFEu && (buf[1] & 0xC0u) == 0x80u) {
              return true;  // fe80::/10 link-local
          }
          // IPv4-mapped (::ffff:a.b.c.d — bytes 0-9 == 0, 10-11 == 0xff) и устаревшая
          // IPv4-compatible форма (::a.b.c.d — bytes 0-11 == 0, уже не all-zero/loopback,
          // проверенные выше) кодируют IPv4-адрес в последних 4 байтах — гоняем ту же
          // проверку октетов, что и для голого IPv4, вместо того чтобы пропускать их не глядя
          // (это и был найденный обход guard'а).
          static const unsigned char kMappedPrefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
          const bool ipv4_mapped = std::memcmp(buf, kMappedPrefix, 12) == 0;
          bool ipv4_compatible = true;
          for (int i = 0; i < 12; ++i) {
              if (buf[i] != 0) {
                  ipv4_compatible = false;
                  break;
              }
          }
          if (ipv4_mapped || ipv4_compatible) {
              return isPrivateIpv4Octets(buf[12], buf[13]);
          }
          return false;
      }
      unsigned int o0 = 0, o1 = 0, o2 = 0, o3 = 0;
      if (std::sscanf(host.c_str(), "%u.%u.%u.%u", &o0, &o1, &o2, &o3) != 4) {
          return false;
      }
      return isPrivateIpv4Octets(o0, o1);
  }
  ```

  Add `#include <arpa/inet.h>` and `#include <cstring>` to the top of `src/webhook/webhook_dispatcher.cpp` (near the other `<c...>` includes).

  Note the `(void)o2; (void)o3;` are unnecessary — `o2`/`o3` are still written by `sscanf` (needed so the format string matches 4 conversions) but genuinely unused after, exactly as in the original code — leave as-is, this already compiled clean before.

- [ ] **Step 2: Declare the moved symbols in `src/webhook/webhook_dispatcher.hpp`**

  In `src/webhook/webhook_dispatcher.hpp`, right after the existing `serializeEvent` declaration (current line 31) and before the `WebhookDispatcher` class comment, add:

  ```cpp
  // Разбор URL и приватность хоста — вынесены наружу ради тестов (SSRF-guard), как signBody/
  // serializeEvent выше. Определения — в .cpp.
  struct ParsedUrl {
      std::string base;
      std::string path;
      std::string host;
      bool valid = false;
  };
  ParsedUrl parseUrl(const std::string& url);
  bool isPrivateHost(const std::string& host);
  ```

- [ ] **Step 3: Build to confirm the split compiles**

  Run (from repo root, using the pinned arm64 builder image — same pattern used throughout this session):
  ```sh
  docker run --rm -v "$(pwd):/w" -w /w resert/telegram-rest-gateway:builder-arm64 bash -lc 'cmake --build build/dev-debug -j"$(nproc)"'
  ```
  Expected: clean build, no new warnings from `webhook_dispatcher.cpp`.

- [ ] **Step 4: Add regression + coverage tests to `tests/webhook_dispatcher_test.cpp`**

  Add near the top of the file, after the existing `using` declarations:
  ```cpp
  using tgw::webhook::isPrivateHost;
  using tgw::webhook::parseUrl;
  ```

  Add these tests after `SerializeThenSignIsStable` and before `LifecycleStartDispatchStopNoActiveHooks`:

  ```cpp
  TEST(WebhookDispatcher, IsPrivateHostBasicRanges) {
      EXPECT_TRUE(isPrivateHost(""));
      EXPECT_TRUE(isPrivateHost("localhost"));
      EXPECT_TRUE(isPrivateHost("127.0.0.1"));
      EXPECT_TRUE(isPrivateHost("10.1.2.3"));
      EXPECT_TRUE(isPrivateHost("192.168.1.1"));
      EXPECT_TRUE(isPrivateHost("172.16.0.1"));
      EXPECT_TRUE(isPrivateHost("172.31.255.255"));
      EXPECT_FALSE(isPrivateHost("172.32.0.1"));
      EXPECT_TRUE(isPrivateHost("169.254.169.254"));
      EXPECT_TRUE(isPrivateHost("0.0.0.0"));
      EXPECT_FALSE(isPrivateHost("8.8.8.8"));
      EXPECT_FALSE(isPrivateHost("example.com"));
  }

  TEST(WebhookDispatcher, IsPrivateHostIpv6Ranges) {
      EXPECT_TRUE(isPrivateHost("::1"));
      EXPECT_TRUE(isPrivateHost("::"));
      EXPECT_TRUE(isPrivateHost("fc00::1"));
      EXPECT_TRUE(isPrivateHost("fd12:3456::1"));
      EXPECT_TRUE(isPrivateHost("fe80::1"));
      EXPECT_FALSE(isPrivateHost("2001:4860:4860::8888"));  // публичный DNS Google — внешний
  }

  // Регрессия на найденный обход guard'а: IPv4-mapped/-compatible IPv6-литералы, кодирующие
  // заблокированный IPv4-адрес, обязаны блокироваться так же, как голый IPv4.
  TEST(WebhookDispatcher, IsPrivateHostIpv4MappedIpv6Blocked) {
      EXPECT_TRUE(isPrivateHost("::ffff:127.0.0.1"));
      EXPECT_TRUE(isPrivateHost("::ffff:169.254.169.254"));
      EXPECT_TRUE(isPrivateHost("::ffff:10.0.0.1"));
      EXPECT_TRUE(isPrivateHost("::127.0.0.1"));  // устаревшая IPv4-compatible форма
      EXPECT_FALSE(isPrivateHost("::ffff:8.8.8.8"));  // публичный IPv4 внутри mapped-нотации
  }

  TEST(WebhookDispatcher, ParseUrlExtractsHostAndPath) {
      const auto p1 = parseUrl("http://example.com:8080/a/b?c=1");
      EXPECT_TRUE(p1.valid);
      EXPECT_EQ(p1.base, "http://example.com:8080");
      EXPECT_EQ(p1.path, "/a/b?c=1");
      EXPECT_EQ(p1.host, "example.com");

      const auto p2 = parseUrl("https://[::ffff:127.0.0.1]:9000/x");
      EXPECT_TRUE(p2.valid);
      EXPECT_EQ(p2.host, "::ffff:127.0.0.1");

      EXPECT_FALSE(parseUrl("not-a-url").valid);
      EXPECT_FALSE(parseUrl("ftp://example.com/").valid);
  }

  // TGW_WEBHOOK_QUEUE_MAX=0 — валидное (не отклоняемое конфигом) значение: dispatch должен
  // молча дропать КАЖДОЕ событие (queue_.size() >= queue_max_ истинно с первого раза), а не
  // падать/висеть — граница из ревью, ранее не покрытая ни одним тестом.
  TEST(WebhookDispatcher, QueueMaxZeroDropsEveryEvent) {
      EmptyStore store;
      tgw::webhook::WebhookRegistry reg(store);
      tgw::webhook::WebhookDispatcher d(reg, /*timeout_ms=*/200, /*queue_max=*/0,
                                        /*ssrf_guard=*/false);
      d.start();
      for (int i = 0; i < 10; ++i) {
          d.dispatch(makeEvent("q" + std::to_string(i)));  // должен дропаться, не падать/висеть
      }
      d.stop();
      SUCCEED();
  }
  ```

- [ ] **Step 5: Run the unit tests**

  ```sh
  docker run --rm -v "$(pwd):/w" -w /w resert/telegram-rest-gateway:builder-arm64 bash -lc 'cmake --build build/dev-debug -j"$(nproc)" && ctest --test-dir build/dev-debug --output-on-failure -R WebhookDispatcher'
  ```
  Expected: all `WebhookDispatcher.*` tests pass, including the 5 new ones. `IsPrivateHostIpv4MappedIpv6Blocked` must fail on the pre-fix code (verify this if in doubt by temporarily reverting Step 1's rewrite) and pass after.

- [ ] **Step 6: Commit**

  ```sh
  git add src/webhook/webhook_dispatcher.cpp src/webhook/webhook_dispatcher.hpp tests/webhook_dispatcher_test.cpp
  git commit -m "fix(webhook): close SSRF-guard bypass via IPv4-mapped IPv6 literals"
  ```

---

### Task 2: MessageSendTracker shutdown drain

**Finding ID:** #2 (Important — no shutdown-drain equivalent to `TdBridge::drainPending()`; a pending `waitFor()` hangs forever on SIGTERM).

**Files:**
- Modify: `src/bridge/message_send_tracker.hpp`
- Modify: `src/bridge/message_send_tracker.cpp`
- Modify: `src/main.cpp`
- Test: `tests/message_send_tracker_test.cpp`

**Interfaces:**
- Produces: `void MessageSendTracker::drainAll()` — public method, called from `main.cpp`'s shutdown signal handler right after `bridge.drainPending()`.

**Root cause:** `TdBridge::drainPending()` (src/bridge/td_bridge.cpp:163-171) force-resolves every in-flight TDLib request with a synthetic error when a shutdown signal arrives, *while the IO loops are still alive* (main.cpp:377-385, called from the `sigwait` thread before `drogon::app().quit()` is queued). `MessageSendTracker` has an analogous "someone might be waiting forever" problem (a `co_await send_tracker.waitFor(...)` in `src/http/message_routes.cpp`) but nothing calls anything on it during shutdown — its `map_` is simply abandoned, so if a request is mid-`waitFor` when SIGTERM arrives, its coroutine (and the HTTP connection, and any claimed Idempotency-Key) never resumes.

- [ ] **Step 1: Add `drainAll()` to `src/bridge/message_send_tracker.hpp`**

  Add to the public section of `MessageSendTracker`, right after `resolveFailed`'s declaration:

  ```cpp
  // Резолвит ВСЕ висящие waitFor() как таймаут (result остаётся nullopt — тот же сигнал, что
  // и у обычного истечения timeout_). Аналог TdBridge::drainPending() (src/bridge/td_bridge.cpp).
  // Вызывать на shutdown ДО quit(), пока IO-петли ещё живы: иначе резюм, замаршаленный через
  // queueInLoop, потеряется и HTTP-хендлер (а с ним и claim Idempotency-Key) зависнет навсегда.
  void drainAll();
  ```

- [ ] **Step 2: Implement `drainAll()` in `src/bridge/message_send_tracker.cpp`**

  Add after `resolveFailed`'s definition, at the end of the file before the closing `}  // namespace tgw::bridge`:

  ```cpp
  void MessageSendTracker::drainAll() {
      // Забираем все узлы разом под общим mutex_ (как claimExpired(max) в CorrelationMap) —
      // единоличный резолв каждого узла сохраняется: он уже извлечён из map_, таймер-колбэк
      // (если ещё не сработал) при claim() увидит пустую запись и станет no-op.
      std::unordered_map<std::int64_t, std::shared_ptr<SendWaitState>> to_drain;
      {
          std::lock_guard<std::mutex> lock(mutex_);
          to_drain.swap(map_);
      }
      for (auto& [id, state] : to_drain) {
          {
              std::lock_guard<std::mutex> lock(state->m);
              if (state->resolved) {
                  continue;  // таймер уже выиграл гонку раньше — no-op
              }
              state->resolved = true;
              // result остаётся nullopt — тот же сигнал, что и у обычного timeout-резолва.
          }
          trantor::EventLoop* loop = state->loop;
          if (loop == nullptr || !loop->isRunning()) {
              if (state->handle) {
                  state->handle.resume();
              }
              continue;
          }
          // state захвачен по значению => жив до resume (как в resolveWith).
          loop->queueInLoop([state]() {
              if (state->handle) {
                  state->handle.resume();
              }
          });
      }
  }
  ```

- [ ] **Step 3: Wire into `src/main.cpp`'s shutdown sequence**

  In `src/main.cpp`, the signal-wait thread (current lines 377-385) reads:
  ```cpp
  std::thread([&bridge, shutdown_signals]() {
      int sig = 0;
      if (sigwait(&shutdown_signals, &sig) != 0) {
          return;  // не должно случаться при валидном наборе
      }
      LOG_INFO << "received signal " << sig << ": draining bridge before quit";
      bridge.drainPending();
      drogon::app().getLoop()->queueInLoop([] { drogon::app().quit(); });
  }).detach();
  ```
  Change the capture list and body to also drain `send_tracker` (declared earlier in `main()`, at current line 203 — it's a local in `main()`'s scope, outlives this thread since the thread is joined implicitly by process exit and `send_tracker` isn't destroyed until `main()` returns, which happens after `drogon::app().run()` returns and the whole shutdown tail completes):

  ```cpp
  std::thread([&bridge, &send_tracker, shutdown_signals]() {
      int sig = 0;
      if (sigwait(&shutdown_signals, &sig) != 0) {
          return;  // не должно случаться при валидном наборе
      }
      LOG_INFO << "received signal " << sig << ": draining bridge before quit";
      bridge.drainPending();
      // MessageSendTracker дренируем тем же способом и на том же шаге: висящие waitFor()
      // (humanize-typing) иначе не резолвятся при shutdown и вешают HTTP-хендлер навсегда
      // (см. комментарий у MessageSendTracker::drainAll). Дёшево вызывать безусловно — при
      // выключенном humanize-typing карта пуста.
      send_tracker.drainAll();
      drogon::app().getLoop()->queueInLoop([] { drogon::app().quit(); });
  }).detach();
  ```

- [ ] **Step 4: Build**

  ```sh
  docker run --rm -v "$(pwd):/w" -w /w resert/telegram-rest-gateway:builder-arm64 bash -lc 'cmake --build build/dev-debug -j"$(nproc)"'
  ```

- [ ] **Step 5: Add a test to `tests/message_send_tracker_test.cpp`**

  Read the existing file first to match its harness pattern exactly (it already has a loop-thread harness for testing `waitFor`/`resolveSucceeded`/timeout — reuse it, don't build a new one). Add a test that:
  1. Starts a `waitFor(some_id, long_timeout)` on the harness's loop (same pattern as the existing timeout test, but with a timeout long enough that it would NOT fire during the test).
  2. Calls `tracker.drainAll()` from the test thread.
  3. Asserts the `waitFor` coroutine resumes promptly with `nullopt` (not by waiting for the long timeout) — i.e. `drainAll()` actually force-resolves it instead of the test having to wait out the real timeout.

  Concretely (adapt exact promise/future plumbing to match whatever pattern the existing timeout test in this file already uses — do not introduce a new harness style):

  ```cpp
  TEST(MessageSendTracker, DrainAllForceResolvesPendingWaitAsTimeout) {
      // ... (reuse this file's existing LoopThread/harness setup — see WaitForTimesOutWithNullopt
      // or equivalent for the exact pattern) ...
      MessageSendTracker tracker;
      // Таймаут заведомо больше времени теста — если бы drainAll() не резолвил, future.wait_for
      // ниже истёк бы по дедлайну ЭТОГО assert, а не по внутреннему таймеру трекера.
      auto future = /* co_await tracker.waitFor(42, 60s) launched on the harness loop, per the
                       file's existing pattern, returning a std::future via promise */;
      // Дать корутине время дойти до await_suspend/tryInsert.
      std::this_thread::sleep_for(50ms);
      tracker.drainAll();
      ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
      const auto result = future.get();
      EXPECT_FALSE(result.has_value());  // nullopt — тот же сигнал, что у обычного таймаута
  }
  ```

  If the existing file has no future/promise plumbing to reuse (verify by reading it first), mirror the `runBuildEvent`-style promise/future wrapper from `tests/context_builder_test.cpp` (current lines 200-219) — same idea, different awaited type.

- [ ] **Step 6: Run tests**

  ```sh
  docker run --rm -v "$(pwd):/w" -w /w resert/telegram-rest-gateway:builder-arm64 bash -lc 'cmake --build build/dev-debug -j"$(nproc)" && ctest --test-dir build/dev-debug --output-on-failure -R MessageSendTracker'
  ```

- [ ] **Step 7: TSan run** (this touches cross-thread resolve logic — worth the extra check)

  ```sh
  docker run --rm -v "$(pwd):/w" -w /w --security-opt seccomp=unconfined resert/telegram-rest-gateway:builder-arm64 bash -lc 'cmake --build build/tsan -j"$(nproc)" && ctest --test-dir build/tsan --output-on-failure -R MessageSendTracker'
  ```

- [ ] **Step 8: Commit**

  ```sh
  git add src/bridge/message_send_tracker.hpp src/bridge/message_send_tracker.cpp src/main.cpp tests/message_send_tracker_test.cpp
  git commit -m "fix(bridge): drain MessageSendTracker on shutdown to prevent hung send-and-wait coroutines"
  ```

---

### Task 3: message_routes.cpp — upload disk leak on invalid type + silently dropped invalid reply_to_message_id

**Finding IDs:** #3 (Important — invalid `?type=` leaves an orphaned upload directory on disk), #4 (Important — invalid `reply_to_message_id` is silently ignored instead of rejected).

**Files:**
- Modify: `src/http/message_routes.cpp`

**Note on finding #15** (send-and-wait coroutine has zero test coverage): per this plan's Global Constraints, `message_routes.cpp` has no live-server test harness and building one is out of scope for this fix cycle (see `route_smoke_test.cpp`'s documented rationale). This task fixes the two concrete bugs found in it; it does not add new test infrastructure for the file. This is a deliberate scope decision, not an oversight — flag it as such if a reviewer raises it, don't silently attempt a bigger test-infra change.

- [ ] **Step 1: Validate `?type=` before writing the upload body to disk**

  In `src/http/message_routes.cpp`, inside the `/v1/chats/{chatId}/files` handler's `uploadTaskQueue().runTaskInQueue([...] { ... })` lambda (current lines 536-620), move the `media_type` validation from its current position (after the body is written, current lines 580 + 615-620) to the very top of the lambda, before any filesystem work:

  Change the lambda's opening from:
  ```cpp
  uploadTaskQueue().runTaskInQueue([&bridge, client_id, upload_dir, chatId, req,
                                    cb = std::move(cb)]() mutable {
      namespace fs = std::filesystem;
      const std::string name = sanitizeUploadFilename(req->getParameter("file_name"));
  ```
  to:
  ```cpp
  uploadTaskQueue().runTaskInQueue([&bridge, client_id, upload_dir, chatId, req,
                                    cb = std::move(cb)]() mutable {
      namespace fs = std::filesystem;
      // ?type= валидируем ДО записи тела на диск: тело может быть до max_upload_bytes (default
      // 64MiB), и раньше при невалидном type оно уже было бы записано, а ветка ошибки ниже не
      // чистила директорию — накопление орфанных файлов на диск (см. фикс ниже, было решение
      // 1.5/C10). Валидировать здесь дёшево: это тот же чистый список значений, что и ниже.
      const std::string media_type = req->getParameter("type");
      if (!media_type.empty() && media_type != "document" && media_type != "photo" &&
          media_type != "video" && media_type != "voice" && media_type != "audio") {
          cb(serviceError("VALIDATION_ERROR", "type must be document|photo|video|voice|audio",
                          drogon::k400BadRequest));
          return;
      }
      const std::string name = sanitizeUploadFilename(req->getParameter("file_name"));
  ```

  Then, further down in the same lambda, remove the now-redundant re-read of `media_type` (current line 580: `const std::string media_type = req->getParameter("type");`) — delete that line since `media_type` is now already in scope from the top of the lambda. Also remove the final `else` branch of the `if/else if` chain (current lines 615-620:
  ```cpp
              } else {
                  cb(serviceError("VALIDATION_ERROR",
                                  "type must be document|photo|video|voice|audio",
                                  drogon::k400BadRequest));
                  return;
              }
  ```
  ) since `media_type` is now pre-validated to always match one of the four branches or be empty (which the first `if` branch already handles via `media_type.empty() || media_type == "document"`) — the `else` is unreachable after Step 1's early-return guard. Just close the `if/else if` chain after the `audio` branch instead.

- [ ] **Step 2: Reject invalid `reply_to_message_id` instead of silently dropping it**

  In the same file, in the `POST /v1/chats/{chatId}/messages` handler (current lines 286-296), the block reads:
  ```cpp
  api::object_ptr<api::inputMessageReplyToMessage> reply;
  std::int64_t replyTo = 0;
  if ((*json)["reply_to_message_id"].isString() &&
      parseId((*json)["reply_to_message_id"].asString(), replyTo) && replyTo != 0) {
      reply = api::make_object<api::inputMessageReplyToMessage>();
      reply->message_id_ = replyTo;
  }
  ```
  Replace with (this runs after the `idem_key` claim and the `makeFormattedText` check, so on the invalid-id error path it must release the idempotency key exactly like the `makeFormattedText` failure branch just above it does):
  ```cpp
  api::object_ptr<api::inputMessageReplyToMessage> reply;
  if ((*json)["reply_to_message_id"].isString()) {
      std::int64_t replyTo = 0;
      if (!parseId((*json)["reply_to_message_id"].asString(), replyTo) || replyTo == 0) {
          if (!idem_key.empty()) {
              IdempotencyCache::instance().release(idem_key);
          }
          cb(serviceError("VALIDATION_ERROR", "invalid reply_to_message_id",
                          drogon::k400BadRequest));
          return;
      }
      reply = api::make_object<api::inputMessageReplyToMessage>();
      reply->message_id_ = replyTo;
  }
  ```
  (Field absent entirely, i.e. `isString()` false — unchanged behavior: no reply, message sends normally. Only a *present-but-invalid* value now gets rejected instead of silently ignored.)

- [ ] **Step 3: Build**

  ```sh
  docker run --rm -v "$(pwd):/w" -w /w resert/telegram-rest-gateway:builder-arm64 bash -lc 'cmake --build build/dev-debug -j"$(nproc)"'
  ```

- [ ] **Step 4: Manual smoke check** (no automated harness available for this file, per Global Constraints — do a quick manual sanity pass instead of skipping verification entirely)

  Read the final diff of both handlers once more and confirm by inspection:
  - Every code path that returns before the coroutine launch still balances `IdempotencyCache::claim`/`release`/`store` (exactly one release-or-store per branch, per Global Constraints).
  - The upload handler's four `if/else if` media-type branches remain unchanged in their content (only the pre-check and dead `else` were touched).

- [ ] **Step 5: Commit**

  ```sh
  git add src/http/message_routes.cpp
  git commit -m "fix(http): validate upload type before writing to disk, reject invalid reply_to_message_id"
  ```

---

### Task 4: Config fail-fast guard — gate on humanize_typing + fix integer overflow

**Finding IDs:** #5 (Important — guard runs even when `humanize_typing` is disabled), #6 (Important — `idle_connection_timeout_seconds * 1000` overflows `int` for large timeouts).

**Files:**
- Modify: `src/config/config.cpp`
- Test: `tests/config_test.cpp`

**Root cause:** `src/config/config.cpp` (current lines 247-257):
```cpp
constexpr int kHumanizeSafetyMarginMs = 2000;
if (c.humanize_max_delay_ms + c.humanize_id_wait_ms + kHumanizeSafetyMarginMs >
    c.idle_connection_timeout_seconds * 1000) {
    throw std::runtime_error(...);
}
```
Two independent bugs: (1) it never checks `c.humanize_typing`, so it can refuse to start a deployment that doesn't use the feature at all; (2) `c.idle_connection_timeout_seconds * 1000` is computed in `int` with no upper-bound check on the parsed env var, so a large-but-reasonable timeout (e.g. 30 days in seconds) overflows and (on gcc/clang in practice) wraps negative, making the guard always trip.

- [ ] **Step 1: Fix both bugs in `src/config/config.cpp`**

  Replace the block (current lines 247-257) with:
  ```cpp
  // Fail-fast guard: худший случай humanize-паузы (max_delay + id_wait, плюс запас на сетевые
  // накладные расходы) не должен превышать idle-таймаут соединения — иначе Drogon/ingress
  // оборвёт соединение раньше, чем гейтвей успеет ответить (см. дизайн-спеку). Актуально
  // ТОЛЬКО когда humanize_typing включён — иначе этот бюджет пауз никогда не расходуется, и
  // блокировать старт сервиса из-за него было бы ложным отказом (напр. оператор поднял
  // TGW_IDLE_CONNECTION_TIMEOUT_SECONDS для WS-idle не думая про эту фичу вовсе).
  if (c.humanize_typing) {
      constexpr std::int64_t kHumanizeSafetyMarginMs = 2000;
      // int64_t: idle_connection_timeout_seconds приходит без верхней границы из env, и
      // умножение на 1000 в int переполняется (signed UB, на практике оборачивается в
      // отрицательное) уже на не экзотичных значениях (>~24 суток) — тогда guard всегда
      // ложно срабатывал бы независимо от реального (щедрого) таймаута.
      const std::int64_t budget_ms = static_cast<std::int64_t>(c.humanize_max_delay_ms) +
                                     static_cast<std::int64_t>(c.humanize_id_wait_ms) +
                                     kHumanizeSafetyMarginMs;
      const std::int64_t idle_ms =
          static_cast<std::int64_t>(c.idle_connection_timeout_seconds) * 1000;
      if (budget_ms > idle_ms) {
          throw std::runtime_error(
              "config: TGW_HUMANIZE_MAX_DELAY_MS + TGW_HUMANIZE_ID_WAIT_MS (+"
              " запас 2000мс) превышает TGW_IDLE_CONNECTION_TIMEOUT_SECONDS — "
              "увеличьте таймаут или уменьшите паузу/окно ожидания");
      }
  }
  ```

- [ ] **Step 2: Build**

  ```sh
  docker run --rm -v "$(pwd):/w" -w /w resert/telegram-rest-gateway:builder-arm64 bash -lc 'cmake --build build/dev-debug -j"$(nproc)"'
  ```

- [ ] **Step 3: Add tests to `tests/config_test.cpp`**

  Read the file first to match its existing `setRequired()`/`set()`/`ConfigTest` fixture pattern exactly (same pattern as the existing `HumanizeTimeoutBudgetExceedsIdleTimeoutThrows` test — this task adds two more tests right next to it). Add:

  ```cpp
  // Fail-fast guard не должен срабатывать, если humanize_typing выключен — бюджет паузы тогда
  // никогда не расходуется, блокировать старт сервиса из-за него было бы ложным отказом.
  TEST_F(ConfigTest, HumanizeTimeoutGuardSkippedWhenFeatureDisabled) {
      setRequired();
      // TGW_HUMANIZE_TYPING не задан (default false).
      set("TGW_HUMANIZE_MAX_DELAY_MS", "50000");
      set("TGW_HUMANIZE_ID_WAIT_MS", "20000");
      set("TGW_IDLE_CONNECTION_TIMEOUT_SECONDS", "10");  // будило бы guard при включённой фиче
      EXPECT_NO_THROW(Config::load());
  }

  // Integer-overflow regression: большой (но легитимный) idle-таймаут раньше переполнял int при
  // умножении на 1000 и ложно валил guard независимо от реального бюджета паузы.
  TEST_F(ConfigTest, HumanizeTimeoutGuardHandlesLargeIdleTimeoutWithoutOverflow) {
      setRequired();
      set("TGW_HUMANIZE_TYPING", "true");
      // Дефолты бюджета (10000+4000+2000=16000мс) укладываются в любой таймаут выше ~16с —
      // 2592000с (30 суток) переполнял бы старую int-арифметику (2592000*1000 > INT_MAX).
      set("TGW_IDLE_CONNECTION_TIMEOUT_SECONDS", "2592000");
      EXPECT_NO_THROW(Config::load());
  }
  ```

- [ ] **Step 4: Run tests**

  ```sh
  docker run --rm -v "$(pwd):/w" -w /w resert/telegram-rest-gateway:builder-arm64 bash -lc 'cmake --build build/dev-debug -j"$(nproc)" && ctest --test-dir build/dev-debug --output-on-failure -R ConfigTest'
  ```

- [ ] **Step 5: Commit**

  ```sh
  git add src/config/config.cpp tests/config_test.cpp
  git commit -m "fix(config): gate humanize-timeout guard on the feature flag, fix integer overflow"
  ```

---

### Task 5: Archiver hardening — SIGTERM media drain, Content-Type XSS, backfill error isolation

**Finding IDs:** #7 (Important — SIGTERM loses queued/in-flight media offloads), #8 (Important — stored XSS via unchecked Content-Type), #9 (Important — one bad message aborts the whole backfill job).

**Files:**
- Modify: `archiver/src/media.ts`
- Modify: `archiver/src/index.ts`

**Interfaces:**
- Produces: `MediaOffloader.drain(timeoutMs: number): Promise<void>` — new public method.
- Consumes: nothing new from other files.

- [ ] **Step 1: Add a `busy` flag and `drain()` to `archiver/src/media.ts`**

  In the `MediaOffloader` class, add a private field next to `private running = true;` (current line 103):
  ```ts
  private running = true;
  private busy = false;
  ```

  In `private async loop()` (current lines 228-261), wrap the `process()` call to track in-flight state:
  ```ts
  private async loop(): Promise<void> {
    while (this.running) {
      const job = this.queue.shift();
      if (!job) {
        await sleep(500);
        continue;
      }
      if (job.notBefore && job.notBefore > Date.now()) {
        this.queue.push(job);
        await sleep(500);
        continue;
      }
      const key = this.jobKey(job);
      this.busy = true;
      try {
        const result = await this.process(job);
        if (result === "retry") {
          this.requeueOrGiveUp(job, key, "download still pending (202)");
        }
      } catch (e: any) {
        const msg = String(e?.message ?? e);
        if (isPermanentError(e)) {
          this.markPermanentFail(key, msg);
        } else {
          console.error(`media: retriable fail ${key}:`, e);
          this.requeueOrGiveUp(job, key, msg);
        }
      } finally {
        this.busy = false;
      }
      await sleep(50);
    }
  }
  ```
  (Only change from the current body: `this.busy = true;` before the `try`, and moving the existing `catch` block's contents unchanged into a `try { ... } catch { ... } finally { this.busy = false; }`.)

  Add a new public method right after `stop()` (current lines 195-197):
  ```ts
  stop() {
    this.running = false;
  }

  /**
   * Ждёт опустошения очереди и завершения текущего in-flight job (best-effort, до timeoutMs),
   * затем останавливает воркер. Вызывать на SIGTERM/SIGINT ДО process.exit — иначе очередь и
   * in-flight job теряются безвозвратно: Kafka-offset для них уже закоммичен к моменту enqueue
   * (enqueue() выполняется синхронно внутри eachMessage до возврата из колбэка), поэтому на
   * рестарте они не передоставляются, а очередь сама по себе только in-memory.
   */
  async drain(timeoutMs: number): Promise<void> {
    const deadline = Date.now() + timeoutMs;
    while ((this.queue.length > 0 || this.busy) && Date.now() < deadline) {
      await sleep(200);
    }
    this.running = false;
    if (this.queue.length > 0 || this.busy) {
      console.error(
        `media: drain timed out with ${this.queue.length} queued job(s)` +
          `${this.busy ? " + 1 in-flight" : ""} — those offloads will be lost`,
      );
    }
  }
  ```

- [ ] **Step 2: Wire `drain()` into `archiver/src/index.ts`'s shutdown handler**

  Add a new env-driven constant near the other top-level `const ... = process.env...` declarations (e.g. right after `const MAX_BODY_BYTES = 64 * 1024;`, current line 21):
  ```ts
  const MEDIA_DRAIN_TIMEOUT_MS = Number(process.env.ARCHIVER_MEDIA_DRAIN_TIMEOUT_MS ?? "30000");
  ```

  Replace the SIGINT/SIGTERM handler (current lines 469-471):
  ```ts
  for (const sig of ["SIGINT", "SIGTERM"] as const) {
    process.on(sig, async () => { try { await consumer.disconnect(); media?.stop(); } finally { process.exit(0); } });
  }
  ```
  with:
  ```ts
  for (const sig of ["SIGINT", "SIGTERM"] as const) {
    process.on(sig, async () => {
      try {
        // Отключаемся от Kafka ПЕРВЫМ — новые события больше не enqueue'ятся, дальше можно
        // дренировать то, что уже в очереди/in-flight, не гоняясь за растущим хвостом.
        await consumer.disconnect();
        if (media) await media.drain(MEDIA_DRAIN_TIMEOUT_MS);
      } finally {
        process.exit(0);
      }
    });
  }
  ```

- [ ] **Step 3: Add a Content-Type allowlist to `archiver/src/media.ts`**

  Add near the top of the file, after the existing type/class declarations and before the `MediaOffloader` class (a good spot: right after `function isPermanentError` and before `export class MediaOffloader`, current line 96-97):
  ```ts
  /**
   * Строгий allowlist медиа-типов для inline-рендера в браузере. Всё, что не совпало (в т.ч.
   * text/html и другие сендер-контролируемые типы, а также image/svg+xml — может нести
   * <script>), уходит как application/octet-stream + Content-Disposition: attachment, чтобы
   * браузер не интерпретировал содержимое как разметку/скрипт при открытии media_url (stored
   * XSS: mime_type в исходном сообщении контролирует отправитель, см. review finding).
   */
  const INLINE_CONTENT_TYPES = new Set([
    "image/jpeg",
    "image/png",
    "image/gif",
    "image/webp",
    "image/bmp",
    "video/mp4",
    "video/webm",
    "video/quicktime",
    "video/x-matroska",
    "video/3gpp",
    "audio/mpeg",
    "audio/ogg",
    "audio/wav",
    "audio/mp4",
    "audio/x-m4a",
    "audio/aac",
    "application/pdf",
  ]);

  function safeContentType(raw: string): { contentType: string; disposition?: string } {
    const type = raw.split(";")[0].trim().toLowerCase();
    if (INLINE_CONTENT_TYPES.has(type)) return { contentType: type };
    return { contentType: "application/octet-stream", disposition: "attachment" };
  }
  ```

  In `private async process(job: MediaJob)`, find the `PutObjectCommand` call (current line 329):
  ```ts
  try {
    await this.s3.send(new PutObjectCommand({ Bucket: this.bucket, Key: key, Body: bytes, ContentType: contentType }));
  } catch (e: any) {
    throw new MediaError(`s3 put: ${String(e?.message ?? e)}`, false);
  }
  ```
  Replace with:
  ```ts
  const { contentType: safeType, disposition } = safeContentType(contentType);
  try {
    await this.s3.send(
      new PutObjectCommand({
        Bucket: this.bucket,
        Key: key,
        Body: bytes,
        ContentType: safeType,
        ...(disposition ? { ContentDisposition: disposition } : {}),
      }),
    );
  } catch (e: any) {
    throw new MediaError(`s3 put: ${String(e?.message ?? e)}`, false);
  }
  ```

- [ ] **Step 4: Isolate per-message errors in `runBackfill`'s inner loop, `archiver/src/index.ts`**

  In `async function runBackfill(...)` (current lines 235-273), the inner `for (const m of page)` loop (current lines 256-259):
  ```ts
  for (const m of page) {
    const row = messageRow(sessionId, m);
    if (row.chat_id && row.message_id) { await store.upsertMessage(row); maybeOffload(row); state.messages_added++; }
  }
  ```
  Replace with:
  ```ts
  for (const m of page) {
    const row = messageRow(sessionId, m);
    if (row.chat_id && row.message_id) {
      try {
        await store.upsertMessage(row);
        maybeOffload(row);
        state.messages_added++;
      } catch (e: any) {
        // Единичный сбой хранилища не должен обрывать весь backfill (все оставшиеся чаты) —
        // логируем и продолжаем со следующим сообщением, как соседний gwGet() уже делает для
        // сбоя страницы (см. try/catch чуть выше по функции).
        console.error(
          `backfill: failed to store message ${row.chat_id}/${row.message_id}, skipping:`,
          e?.message ?? e,
        );
      }
    }
  }
  ```

- [ ] **Step 5: Typecheck / build the sidecar**

  ```sh
  cd archiver && npm run build
  ```
  (Match whatever build/typecheck command this package actually defines — check `archiver/package.json`'s `scripts` first; use `tsc --noEmit` if there's no separate build step, or the project's existing `npm run build`/CI `node-build` step equivalent.)

- [ ] **Step 6: Commit**

  ```sh
  git add archiver/src/media.ts archiver/src/index.ts
  git commit -m "fix(archiver): drain media queue on shutdown, sanitize stored Content-Type, isolate per-message backfill errors"
  ```

---

### Task 6: Helm hardening — ingress whitelist guard, tools securityContext, CPU limits

**Finding IDs:** #10 (Important — base chart has no `whitelist-source-range` guard, `/ui`+`/metrics` can leak to the open internet), #11 (Important — archiver/mcp Deployments have no `securityContext`), #12 (Important — no CPU limit anywhere, noisy-neighbor risk).

**Files:**
- Modify: `deploy/helm/telegram-rest-gateway/templates/_helpers.tpl`
- Modify: `deploy/helm/telegram-rest-gateway/templates/ingress.yaml`
- Modify: `deploy/helm/telegram-rest-gateway/values.yaml`
- Modify: `deploy/helm/telegram-rest-gateway-tools/templates/archiver.yaml`
- Modify: `deploy/helm/telegram-rest-gateway-tools/templates/mcp.yaml`
- Modify: `deploy/helm/telegram-rest-gateway-tools/values.yaml`

- [ ] **Step 1: Add a fail-fast ingress guard to `deploy/helm/telegram-rest-gateway/templates/_helpers.tpl`**

  Add at the end of the file, after the existing `tgw.accountHost` define block:
  ```yaml
  {{/* Guard: ingress без whitelist-source-range выставляет /ui (форма логина аккаунта) и
       /metrics (без Bearer-auth) в открытый интернет — они полагаются ИСКЛЮЧИТЕЛЬНО на
       IP-allowlist (см. DEPLOY.md). Тот же fail-fast принцип, что у tgw.imageTag выше: явная
       ошибка на старте вместо тихой дыры в проде. allowOpenIngress — осознанный опт-аут (напр.
       если доступ ограничен иначе — mTLS/VPN/NetworkPolicy). */}}
  {{- define "tgw.ingressGuard" -}}
  {{- if and .Values.ingress.enabled (not .Values.ingress.allowOpenIngress) (not (hasKey .Values.ingress.annotations "nginx.ingress.kubernetes.io/whitelist-source-range")) -}}
  {{- fail "ingress.enabled=true без nginx.ingress.kubernetes.io/whitelist-source-range: /ui и /metrics не защищены Bearer-auth и полагаются на IP-allowlist (см. DEPLOY.md). Задай аннотацию, либо прими риск явно через ingress.allowOpenIngress=true." -}}
  {{- end -}}
  {{- end -}}
  ```

- [ ] **Step 2: Invoke the guard from `deploy/helm/telegram-rest-gateway/templates/ingress.yaml`**

  Change the file's opening line from:
  ```yaml
  {{- if .Values.ingress.enabled }}
  {{- range $account := .Values.accounts }}
  ```
  to:
  ```yaml
  {{- if .Values.ingress.enabled }}
  {{- include "tgw.ingressGuard" . }}
  {{- range $account := .Values.accounts }}
  ```

- [ ] **Step 3: Add `allowOpenIngress` default to `deploy/helm/telegram-rest-gateway/values.yaml`**

  In the `ingress:` block (current lines 69-81), after the `annotations:` block and before `hostTemplate:`, add:
  ```yaml
  ingress:
    enabled: false
    className: ""
    annotations:
      nginx.ingress.kubernetes.io/proxy-read-timeout: "90"
      nginx.ingress.kubernetes.io/proxy-send-timeout: "90"
    # Явный опт-аут guard'а (см. _helpers.tpl: tgw.ingressGuard). Без
    # nginx.ingress.kubernetes.io/whitelist-source-range в annotations выше, /ui (форма логина)
    # и /metrics обслуживаются БЕЗ Bearer-auth — helm install/template откажет с понятной
    # ошибкой, если это не установлено в true осознанно (напр. доступ ограничен иначе).
    allowOpenIngress: false
    hostTemplate: "tg-{sessionId}.example.com"
    ...
  ```
  (Keep the rest of the block — `hostTemplate`/`tls` — unchanged; only inserting `allowOpenIngress: false` with its comment between `annotations` and `hostTemplate`.)

- [ ] **Step 4: Add `cpu` limits to `deploy/helm/telegram-rest-gateway/values.yaml`**

  Change the `resources:` block (current lines 83-89) from:
  ```yaml
  resources:
    requests:
      cpu: 50m
      memory: 64Mi
    limits:
      memory: 256Mi
  ```
  to:
  ```yaml
  resources:
    requests:
      cpu: 50m
      memory: 64Mi
    limits:
      # CPU-limit отсутствовал: при инварианте «одна сессия = один под» интенсивная нагрузка
      # одного аккаунта (burst медиа, catch-up после простоя) могла занять весь CPU ноды и
      # затормозить другие сессии на ней (noisy neighbor) — см. review finding.
      cpu: 500m
      memory: 256Mi
  ```

- [ ] **Step 5: Add `securityContext` + `cpu` limits to `deploy/helm/telegram-rest-gateway-tools/values.yaml`**

  In the `archiver:` block, change `resources:` (current lines 49-51) from:
  ```yaml
  resources:
    requests: { cpu: 25m, memory: 64Mi }
    limits: { memory: 256Mi }
  ```
  to:
  ```yaml
  resources:
    requests: { cpu: 25m, memory: 64Mi }
    limits: { cpu: 500m, memory: 256Mi }  # CPU-limit добавлен — см. review finding (noisy neighbor)
  # Контейнер бежит от USER node (Dockerfile) — тот же класс hardening'а, что у основного
  # gateway-чарта (deploy/helm/telegram-rest-gateway/values.yaml securityContext), которого тут
  # раньше не было вовсе (см. review finding).
  securityContext:
    runAsNonRoot: true
    runAsUser: 1000
    runAsGroup: 1000
    allowPrivilegeEscalation: false
    capabilities:
      drop: ["ALL"]
  ```

  In the `mcp:` block, change `resources:` (current lines 70-72) from:
  ```yaml
  resources:
    requests: { cpu: 25m, memory: 48Mi }
    limits: { memory: 128Mi }
  ```
  to:
  ```yaml
  resources:
    requests: { cpu: 25m, memory: 48Mi }
    limits: { cpu: 250m, memory: 128Mi }  # CPU-limit добавлен — см. review finding
  # Тот же hardening, что у archiver выше — контейнер бежит от USER node (Dockerfile).
  securityContext:
    runAsNonRoot: true
    runAsUser: 1000
    runAsGroup: 1000
    allowPrivilegeEscalation: false
    capabilities:
      drop: ["ALL"]
  ```

- [ ] **Step 6: Reference the new `securityContext` values from the templates**

  In `deploy/helm/telegram-rest-gateway-tools/templates/archiver.yaml`, in the `containers:` entry (current lines 47-50), add a `securityContext` field right after `imagePullPolicy` and before `ports`:
  ```yaml
        - name: archiver
          image: "{{ .Values.archiver.image.repository }}:{{ required "archiver.image.tag обязателен: CI публикует immutable short-sha теги, см. docs/CICD.md" .Values.archiver.image.tag }}"
          imagePullPolicy: {{ .Values.archiver.image.pullPolicy }}
          securityContext:
            {{- toYaml .Values.archiver.securityContext | nindent 12 }}
          ports:
  ```

  In `deploy/helm/telegram-rest-gateway-tools/templates/mcp.yaml`, in the `containers:` entry (current lines 29-32), add the same right after `imagePullPolicy` and before `ports`:
  ```yaml
        - name: mcp
          image: "{{ $.Values.mcp.image.repository }}:{{ required "mcp.image.tag обязателен: CI публикует immutable short-sha теги, см. docs/CICD.md" $.Values.mcp.image.tag }}"
          imagePullPolicy: {{ $.Values.mcp.image.pullPolicy }}
          securityContext:
            {{- toYaml $.Values.mcp.securityContext | nindent 12 }}
          ports:
  ```

- [ ] **Step 7: Validate the charts render**

  ```sh
  helm template deploy/helm/telegram-rest-gateway \
    --set image.tag=test --set ingress.enabled=true \
    --set ingress.annotations."nginx\.ingress\.kubernetes\.io/whitelist-source-range"="10.0.0.0/8" \
    > /dev/null && echo "gateway chart OK (with whitelist annotation)"

  # Confirm the guard actually fires without the annotation:
  helm template deploy/helm/telegram-rest-gateway --set image.tag=test --set ingress.enabled=true 2>&1 \
    | grep -q "whitelist-source-range" && echo "guard fires as expected"

  helm template deploy/helm/telegram-rest-gateway-tools \
    --set archiver.image.tag=test --set mcp.image.tag=test \
    --set mcp.accounts[0].sessionId=123 \
    > /dev/null && echo "tools chart OK"
  ```
  Expected: first and third commands succeed silently; the guard-check command prints the `fail` error text and `guard fires as expected` (the `grep` matches the error message helm prints to stderr — redirect `2>&1` as shown).

- [ ] **Step 8: Commit**

  ```sh
  git add deploy/helm/telegram-rest-gateway/templates/_helpers.tpl \
          deploy/helm/telegram-rest-gateway/templates/ingress.yaml \
          deploy/helm/telegram-rest-gateway/values.yaml \
          deploy/helm/telegram-rest-gateway-tools/templates/archiver.yaml \
          deploy/helm/telegram-rest-gateway-tools/templates/mcp.yaml \
          deploy/helm/telegram-rest-gateway-tools/values.yaml
  git commit -m "fix(helm): fail-fast ingress whitelist guard, harden tools securityContext, add CPU limits"
  ```

---

### Task 7: Webhook reply-chain owner-check bypass at chain_limit=0

**Finding ID:** #13 (Important — at `chain_limit=0`, `buildEvent`'s reply-chain loop never runs, so the owner-verification check is skipped entirely; not exploitable today since `chain_limit` is hardcoded to 20 at every call site, but completely untested and would silently reactivate the moment it becomes configurable).

**Files:**
- Modify: `src/webhook/context_builder.cpp`
- Test: `tests/context_builder_test.cpp`

**Root cause:** In `buildEvent` (`src/webhook/context_builder.cpp`, current lines 52-91), the owner-verification (`senderIsOwner(parent, owner_id)`) only happens *inside* the `while (cur != 0 && hops < chain_limit)` loop, at `hops == 0`. If `chain_limit <= 0`, the loop body never executes even though `parent_id` is present, so control falls through to the trigger_reason fallback (current lines 94-97) which sets `trigger_reason = "reply"` from `det.reason` alone — no owner check ever ran.

- [ ] **Step 1: Add a fail-closed guard in `src/webhook/context_builder.cpp`**

  In `buildEvent`, right before the existing `bool truncated = false;` line (current line 52), add:
  ```cpp
  // chain_limit<=0 означает, что цикл ниже не сделает ни одного шага — значит, для
  // reply_pending НЕВОЗМОЖНО проверить автора первого родителя. Тот же отказ, что и для
  // reply_pending без родителя вовсе (см. ветку else ниже) — иначе (см. review finding)
  // trigger_reason="reply" выставлялся бы без единой проверки владельца.
  if (det.reply_pending && chain_limit <= 0) {
      co_return std::nullopt;
  }
  bool truncated = false;
  ```

- [ ] **Step 2: Build**

  ```sh
  docker run --rm -v "$(pwd):/w" -w /w resert/telegram-rest-gateway:builder-arm64 bash -lc 'cmake --build build/dev-debug -j"$(nproc)"'
  ```

- [ ] **Step 3: Add a regression test to `tests/context_builder_test.cpp`**

  Add after `ReplyPendingFirstGetMessageErrorReturnsNullopt` (the last test in the file):
  ```cpp
  // chain_limit=0: цикл reply-chain не делает ни одного шага, значит проверить владельца
  // первого родителя невозможно — триггер обязан остаться неподтверждённым (регресс на
  // найденный обход: раньше trigger_reason="reply" выставлялся без единой проверки владельца).
  // НЕТ ChainDriver — намеренно: при регрессе buildEvent послал бы getMessage, которого некому
  // обслужить, и тест зависал бы до истечения future.wait_for(2s), а не тихо проходил.
  TEST(ContextBuilder, ReplyPendingChainLimitZeroReturnsNullopt) {
      BridgeHarness h{BridgeConfig{}};
      const auto cid = h.bridge().createClientId();

      const auto incoming = makeMsg(4200, -100500, 999, 4100);
      DetectResult det;
      det.triggered = false;
      det.reply_pending = true;
      det.reason = TriggerReason::Reply;

      auto future = runBuildEvent(h, cid, *incoming, det, /*owner=*/111, "sess", 1730000000,
                                  /*chain_limit=*/0);
      ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
      const auto result = future.get();

      EXPECT_FALSE(result.has_value());
  }
  ```

- [ ] **Step 4: Run tests**

  ```sh
  docker run --rm -v "$(pwd):/w" -w /w resert/telegram-rest-gateway:builder-arm64 bash -lc 'cmake --build build/dev-debug -j"$(nproc)" && ctest --test-dir build/dev-debug --output-on-failure -R ContextBuilder'
  ```
  Expected: all `ContextBuilder.*` tests pass, including the new one, within its 2s timeout (confirming the fix returns immediately rather than the (buggy, pre-fix) code path that would hang waiting for an unscripted `getMessage` response).

- [ ] **Step 5: Commit**

  ```sh
  git add src/webhook/context_builder.cpp tests/context_builder_test.cpp
  git commit -m "fix(webhook): fail closed on reply owner-check when chain_limit<=0"
  ```

---

## Final Steps (after all 7 tasks)

1. Full build + full test suite (dev-debug):
   ```sh
   docker run --rm -v "$(pwd):/w" -w /w resert/telegram-rest-gateway:builder-arm64 bash -lc 'cmake --build build/dev-debug -j"$(nproc)" && ctest --test-dir build/dev-debug --output-on-failure'
   ```
2. Full TSan suite:
   ```sh
   docker run --rm -v "$(pwd):/w" -w /w --security-opt seccomp=unconfined resert/telegram-rest-gateway:builder-arm64 bash -lc 'cmake --build build/tsan -j"$(nproc)" && ctest --test-dir build/tsan --output-on-failure'
   ```
3. `clang-format --dry-run --Werror` over all touched `.cpp`/`.hpp` files (or run `clang-format -i` via the pinned `python:3.12-slim` + `clang-format==18.1.8` container pattern used earlier this session, then re-verify the diff didn't change logic).
4. `helm template` both charts once more with representative `--set` overrides to catch any YAML/templating mistake.
5. Whole-branch code review (per subagent-driven-development's final-review step) before opening the PR.
