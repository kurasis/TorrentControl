# Обзор TorrentControl и дальнейшие шаги

Проверяемая база: импортированный коммит `9faefdcdf4c223c347779176c457295cda617303`.
Архив импортирован без изменения исходников; приложенная спецификация сохранена
в [development-spec.md](development-spec.md). Требования документа использованы
для оценки проекта, а не как отдельное поручение реализовать все этапы.

Приоритетные исправления первого этапа реализованы; поведение и регрессионные
проверки описаны в [m2-stabilization.md](m2-stabilization.md). Ниже сохранены
первоначальные наблюдения об импортированной базе.

Следующий пакет добавляет [нативную Windows-проверку](windows-native-validation.md):
реальные системные диалоги, v1/v2/hybrid create/open/verify, побайтное повторение
проекта, сбой renderer и восстановление snapshot, перезапуск настроек и
отсутствующий Runtime. Ручные проверки доступности, DPI и чистой установки
остаются отдельными release gates. [Редактор и registry M3](m3-metadata-editor.md)
добавлены следующим пакетом; далее — сетевые diagnostics и N/S fixtures.

## Текущее состояние

Изучены исходники ядра, сервиса, bridge, Windows host, frontend, инструменты,
тесты, конфигурация сборки/CI и документация. Это реализация M2, требующая
стабилизации перед расширением:

- C++20, libtorrent 2.1.2 и собственный конвейер однопроходного v1/v2/hybrid
  хеширования. Padding создаётся в памяти; виртуальные источники не копируются.
- Bencode сохраняет бинарные ключи, большие целые и исходный `info`; outer-only
  редактирование сохраняет идентификаторы. Есть проверка payload и magnet.
- Сервис владеет draft, сканированием, профилями, batch, очередью и persistence.
  Задания получают снимок настроек; frontend восстанавливается через snapshot.
- Win32/WebView2 host ограничивает навигацию и привилегированные операции.
  Frontend — bundled HTML/CSS/JavaScript; сборки TypeScript сейчас нет.
- Catch2, независимый Python verifier, интеграционные и Playwright тесты,
  Windows/Linux CI уже подготовлены. Наличие теста в acceptance.md не является
  доказательством прохождения этого теста на Windows в текущем импорте.
- DiagnosticsService и установщик отсутствуют; registry и редактор описаны
  в [m3-metadata-editor.md](m3-metadata-editor.md).
  Создание torrent не открывает libtorrent session и не требует сети.

## Выполненная проверка облачной среды

Среда Linux: GCC 14.2, Python 3.12, Node 24, CMake 3.31.6, Ninja 1.11.1.4.
Vcpkg закреплён на `3cbc1db4d867ec83c89fba4c461321c11f78b5e3`, как в manifest/CI.
Сборка использует preset `linux-release` с warnings-as-errors.

- Повторное выполнение полного setup script успешно: configure, C++ build,
  npm ci. Все 103 файла исходного архива совпадают по SHA-256 после импорта.
- CTest: 108/108 прошли (107 Catch2 cases и integration_proof), без failures.
  В integration_proof — 24 Python tests: 23 прошли, UNC test пропущен на Linux.
- Playwright/Chromium: 11/11 прошли, включая hostile names, 100 000 записей,
  keyboard navigation, переключение режимов и масштаб 100/150/200%.
- Три отдельные проверки подтвердили дефекты batch private, восстановления
  resolved piece size и cleanup при фактическом отказе записи. Это наблюдения
  о дефектах, а не успешные regression tests.

UI-тесты используют mock bridge. Они не проверяют Windows shell, настоящие
файловые диалоги, DPAPI или поведение WebView2. Запрос статуса GitHub Actions
через API получил HTTP 403; результат удалённого CI здесь не подтверждён.

Для повторения после выполнения setup script:

```bash
export PATH=/workspace/setup-tools/bin:$PATH
export VCPKG_ROOT=/workspace/deps/vcpkg
export VCPKG_DISABLE_METRICS=1 VCPKG_MAX_CONCURRENCY=4
export XDG_CACHE_HOME=/workspace/cache
cd /workspace/TorrentControl
cmake --preset linux-release
cmake --build --preset linux-release --parallel 4
ctest --preset linux-release --parallel 4
cd tests/ui
TC_CHROMIUM=/usr/bin/chromium npm test -- --workers=4
```

## Приоритетные проблемы

### P1 — batch обходит правила private torrent (исправлено)

`src/service/src/app_service.cpp`: `start_create()` вызывает `validate_draft()`,
но `start_batch()` сразу преобразует draft в CreateOptions и ставит задания
в очередь. `run_create()` не проверяет правила профиля. В результате private
с публичными preset-трекерами либо без авторизованного трекера можно создать
через batch, хотя обычное создание запрещено.

Подтверждено отдельной программой, связанной с собранным сервисом: добавить
маленький файл, включить `private=true` при public profile, выполнить validation
и затем single-mode batch. `canCreate=false` с PRIVATE_PUBLIC_TRACKER, но
batch job завершился Succeeded. Независимый bdecode результата подтвердил
`info.private=1` и 10 публичных tracker tiers.

Следующий шаг: выделить общую проверку настроек и применять её к каждому
снимку batch перед enqueue. Проверку назначения и manifest проводить отдельно
для каждого элемента. Добавить проверки private + public preset, отсутствующего
трекера и запрещённых профилем web seeds для обоих способов создания.

### P1 — сохранение профиля использует отключённый prompt (исправлено)

`frontend/app.js`: `saveProfile()` вызывает `window.prompt()`, а
`src/app/windows/webview_host.cpp` отключает default script dialogs и не
обрабатывает ScriptDialogOpening. Имя профиля получить нельзя, поэтому действие
завершается без сохранения. Clipboard fallback также использует prompt.

Это статически установленное несоответствие; требуется проверка в WebView2.
Следующий шаг: форма имени в HTML dialog, клавиатурное управление и понятная
ошибка. Проверить Save Profile и clipboard fallback в настоящем Windows host.

### P2 — проект не восстанавливает resolved piece size (исправлено)

`src/service/src/storage.cpp`: `project_json()` сохраняет
`pieceSizePolicy.resolvedPieceLength`, но `load_project()` читает только draft.
Для автоматического выбора draft содержит 0: открытие проекта снова запускает
автоматическую политику и не сохраняет прежнее решение при изменении данных
или версии политики.

Подтверждено: `save_project(path, Draft{}, 65536)` записал resolved value 65536;
`load_project(path).piece_length` вернул 0. Сохранённое решение игнорируется.

Следующий шаг: хранить и восстанавливать версию политики и resolved value,
определить явное действие для повторного auto-расчёта; проверить round-trip
проекта с автоматическим размером, а не только с ручным.

### P2 — ошибка фактической записи может оставить временный torrent (исправлено)

`src/core/src/output.cpp`: `write_new_file(temp, bytes)` находится перед try,
который удаляет temp при ошибке. Ошибка write/flush/close проходит мимо cleanup.
Существующий тест «write failure» бросает исключение через hook уже после записи
и этот сценарий не покрывает. Предыдущее назначение сохраняется, но temp может
остаться на диске.

Подтверждено на Linux: ограничение `RLIMIT_FSIZE=16`, SIGXFSZ игнорируется;
`commit_output()` валидного torrent вернул `File too large`, а в папке остался
файл `.write-failure.torrent.tc-*.tmp`. Ограничение действовало только внутри
отдельного процесса проверки и было восстановлено после операции.

Следующий шаг: cleanup должен покрывать создание и запись temp с учётом
принадлежности файла; добавить реальный отказ I/O, например EFBIG/ENOSPC,
и проверить отсутствие temp и сохранность прежнего результата.

### P2 — responsiveness и лимиты bridge требуют проверки настоящего сервиса

Windows host выполняет dispatcher на UI thread. В этом пути есть preflight
под mutex, синхронный batch scan, разбор импортированного torrent и поиск по
всему manifest для каждой страницы. Тест 100 000 записей использует mock и
проверяет DOM; он не измеряет время этих операций.

Snapshot/job verification отправляют полные коллекции; DisplayBudget в
`src/bridge/src/bencode_json.cpp` заменяет отдельные значения маркерами, но
сохраняет все элементы длинного списка. Лимит входящих сообщений не ограничивает
размер исходящего JSON.

Следующий шаг: фоновые операции с revision, кеширование preflight/filtered
indices, пагинация verification и metadata, предел исходящих сообщений.
Измерить задержки UI и память на реальных 100 000 файлах через native bridge.

## Дополнительное усиление перед выпуском

Крупные draft/profile/batch/overview коллекции теперь доступны через
[нативную пагинацию](model-pagination.md), правка одной строки сохраняет
незагруженные данные. Ограничены страницы UI и выдача snapshot; полная память
нативных моделей и frontend job history остаётся отдельным этапом измерений.

- Проверка недоверенного metainfo: общие правила безопасных Windows-компонентов,
  path collisions, checked arithmetic и верхняя граница piece length перед
  преобразованием в int. Fuzz bencode/metainfo/verify под ASan/UBSan.
- `JobScheduler::run_verify()` перехватывает только CoreError. Ошибка стандартной
  библиотеки, например bad_alloc, может выйти из worker thread. Нужна обработка
  исключений с терминальным статусом задания и корректным освобождением счётчика.
- Magnet по умолчанию включает трекеры. Перед копированием/экспортом private
  magnet нужен явный выбор и предупреждение о passkey, а не только redaction
  при экспорте профиля. Проверить URL с разными форматами секретов.
- [Сохранение настроек и минимальный Runtime](settings-runtime-reliability.md):
  ошибки записи возвращаются в UI без изменения состояния, режим только на
  текущую сессию обозначен постоянно. До открытия UI проверяется минимальная
  версия 113.0.1774.30. Чистая установка и offline Runtime остаются в M4.
- В архиве нет лицензии самого приложения. Перед распространением определить
  её, дополнить third-party notices, SBOM и происхождение встроенного каталога.
  Документация engine-notes также требует сверки: info.source уже применяется
  сервисом, хотя один раздел относит его к будущему M3.

## Предлагаемый порядок разработки

1. **Стабилизировать M2 — исправления реализованы.** Исправить перечисленные P1 и воспроизводимость проекта,
   cleanup и обработку ошибок verify. Критерий: локальные suites и Windows CI
   проходят; в настоящем WebView2 можно сохранить/переоткрыть проект и профиль,
   private-проверки одинаковы для single и batch. Зафиксировать эти сценарии
   регрессиями и обновить acceptance evidence.
2. **M3: редактор и registry — реализованы.** Реестр полей с типом, областью
   outer/info, влиянием на infohash и правилами безопасности. Outer edits
   сохраняют raw info; info edits показывают изменение идентификаторов;
   structural edits требуют rebuild; signatures не удаляются молча. Binary
   keys/large integers не теряются. BEP 17 httpseeds редактируются на outer layer.
3. **M3: диагностика — реализована.** Отдельный DiagnosticsService для HTTP/UDP
   trackers и BEP 19/17 web seeds; явный запуск, ограниченные таймауты и
   параллелизм, cancellation, redaction и результаты с временем проверки.
   Начать с локальных HTTP/UDP fixtures N/S, не обращаться к публичным трекерам
   в тестах. Разделить «URL записан» и «сервер проверен» в интерфейсе.
   Реализация, ограничения и fixtures: [отчёт диагностики](m3-network-diagnostics.md).
4. **Performance и совместимость — начаты, gates ещё открыты.** Native UI benchmark, single-pass
   instrumentation, память/IO на больших деревьях и slow/UNC sources. Проверить
   созданные v1/v2/hybrid torrents в двух независимо реализованных движках;
   Python reference полезен, но совместимость с клиентами пока не доказана.
   Первые реальные native measurements, кеширование и ограничения второго
   клиента: [отчёт этапа](native-performance-compatibility.md).
   Bridge перенесён с потока окна на последовательный worker; настоящий
   WebView2 проверяет heartbeat на 100 000 файлах:
   [отзывчивость native host](async-native-bridge.md).
   Добавлены лимит исходящих сообщений и страницы результатов, журналов и
   больших полей: [ограничения bridge](bounded-outgoing-bridge.md).
5. **M4: распространение.** Offline WebView2 prerequisite, Windows 10/11 clean
   machine tests без IDE/Python/vcpkg, установщик/удаление, license/SBOM/notices,
   signing и release artifacts. Выпускать после предыдущих acceptance gates.

Первый пакет исправлений M2 реализован, см. [отчёт стабилизации](m2-stabilization.md).
Проверки Windows и последующие этапы оцениваются отдельно; реализация всей
спецификации здесь не заявляется завершённой.

Пакет [памяти при создании](creation-memory.md) ограничивает payload-буферы,
освобождает промежуточные хеши до сериализации и сохраняет измерения реальных
create/verify fixtures. Следом — отменяемое чтение Windows и медленный I/O.

Пакет [отменяемого payload I/O](cancellable-payload-io.md) передаёт stop до
reader, отменяет ожидающие Windows reads через CancelIoEx и проверяет безопасное
завершение/сохранность предыдущего output. Реальные remote SMB faults и
независимое подтверждение v2 остаются release gates.

Пакет [дерева процессов и SMB](process-memory-smb.md) измеряет одинаковые
AppService-сценарии в headless и настоящем Windows/WebView2, включая дочерние
процессы и очистку истории. Добавлены реальные TCP/SMB-сбои Linux CIFS/Samba:
ожидающее чтение, задержанное открытие, отключение сервера и успешный повтор.
Исправлены отказ проверки большого hybrid torrent и удержание удалённой native
истории в renderer. Следующий [Windows SMB пакет](windows-smb-faults.md)
проверяет Windows redirector и настоящее закрытие WebView2 при зависших
чтении/открытии через отдельную Linux VM. [Пакет длительных сессий](long-session-memory.md)
добавляет по 3000 реальных заданий в одном headless/WebView2 процессе,
повторное использование worker pool, освобождение входных данных и страницы
истории. Физический удалённый NAS, многодневные сессии с большими отчётами и
произвольные storage-driver stalls остаются gates. Далее — подтверждённый
импорт в два независимых штатных клиента, расширение security-проверок,
ручная доступность/DPI и распространение M4.
