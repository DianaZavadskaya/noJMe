# Migration Plan

## Steps

- [x] **1. Инвентаризация** — составить список всех модулей, зависимостей, точек входа/выхода
  - Результат: [docs/INVENTORY.md](INVENTORY.md)

- [x] **2. Выбор стратегии** — переписать полностью ("big bang") или постепенно ("strangler fig")
  - Результат: [docs/STRATEGY.md](STRATEGY.md) — выбрана стратегия Strangler Fig

- [x] **3. Покрытие тестами** — зафиксировать текущее поведение тестами до миграции
  - Результат: baseline захвачен для 9 JAR (`make baseline`), регрессии проверяются через `make test`
  - Документация: [docs/ORACLE.md](ORACLE.md)

- [ ] **4. Миграция по слоям** — начать с утилит и моделей, закончить UI/API

  **Phase 1 (Haiku 4.5):**
  - [x] utils → `js/utils/utils.mjs`
  - [x] debug_var → `js/jvm/debug_var.mjs`
  - [x] stb_image_impl → `js/utils/stb_image_impl.mjs`
  - [x] drm_bypass → `js/jvm/drm_bypass.mjs`
  - [x] method_cache → `js/jvm/method_cache.mjs`
  - [x] midi → `js/midi/midi.mjs`
  - [x] sdl_headless → `js/sdl/sdl_headless.mjs`

  **Phase 2 (Sonnet 4.6):**
  - [x] classfile → `js/jvm/classfile.mjs`
  - [x] rms → `js/midp/rms.mjs`

  **Phase 3 (Sonnet 4.6):**
  - [x] media → `js/midp/media.mjs`
  - [x] stubs → `js/jvm/stubs.mjs`

  **Phase 4 (Sonnet 4.6):**
  - [x] threads → `js/jvm/threads.mjs`
  - [x] heap → `js/jvm/heap.mjs`

  **Phase 5 (Sonnet 4.6):**
  - [x] jvm → `js/jvm/jvm.mjs`
  - [x] execute → `js/jvm/execute.mjs`

  **Phase 6 (Sonnet 4.6):**
  - [x] graphics → `js/midp/graphics.mjs`
  - [x] form → `js/midp/form.mjs`

  **Phase 7 (Sonnet 4.6):**
  - [x] sdl_graphics → `js/sdl/sdl_graphics.mjs`
  - [x] sdl_backend_stubs → `js/libretro/sdl_backend_stubs.mjs`
  - [x] main → `js/main.mjs`
  - [x] libretro → `js/libretro/libretro.mjs`

  **Phase 8 (Opus 4.7):**
  - [x] opcodes → `js/jvm/opcodes.mjs`

  **Phase 9 (Sonnet 4.6):**
  - [ ] display → `js/midp/display.mjs`
  - [ ] nokia_m3d → `js/jvm/nokia_m3d.mjs`

  **Phase 10 (Sonnet 4.6):**
  - [ ] native → `js/jvm/native.mjs`

  **Phase 11 (Sonnet 4.6):**
  - [ ] mobile3d → `js/midp/mobile3d.mjs`
