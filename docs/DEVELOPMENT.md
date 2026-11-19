# Development and packaging notes

## Build-time vs runtime responsibilities

CMake stages a verified release from a local directory or HTTPS ZIP. Optionally it retrieves pinned llama.cpp and builds the `assistant-runtime` worker with a local CPU/Metal backend. It installs the plugin, the executable and staged model assets under `Plugins/` (or `PlugIns/`, depending on where the host relocates its plugins). The UI finds these relative to the running application, or lets users choose paths manually.

No network server, socket client, background Python process or online model inference is present.

## Integration

The original `ExampleView` example was a data-specific view supporting Points datasets and depending on its sibling `Common/`. `ChatAssistantPlugin` is a new dataset-independent `ViewPlugin`. It requires no `PointData` target, no `Common/`, no data-drop callback, and no original example metadata. It is created from ManiVault's View menu, not by selecting a dataset.

`Q_PLUGIN_METADATA` is declared exactly once, on `ChatAssistantPluginFactory`. Build via `mv_project_defaults()` and `mv_handle_plugin_config()` so the name and metadata match the SDK conventions. Build with the exact Qt/toolchain from the target ManiVault version; do not mix Qt installs.

## Model path and schema

A selected release folder contains `assistant.gguf` and `knowledge.db` or `model-shards/*.gguf` and `knowledge.db`. `KnowledgeIndex` checks schema version 1. The plugin does not mutate the database, merge weights, fine-tune or download anything at runtime.

The model selection setting permits switching to a larger or smaller compatible GGUF without recompiling the UI. `llama-completion` has CLI flags that vary between upstream revisions; the wrapper probes `--help` and passes optional flags only if advertised. For reliable one-shot prompts, pin a tested upstream commit rather than tracking `master`.

## Build verification checklist

1. Configure in the real ManiVault development bundle with Qt Widgets, Core and Sql.
2. Confirm the `assistant-runtime` target and plugin library compile and install.
3. Inspect the installed resource paths and dynamic library dependencies on macOS, Windows and Linux.
4. Start ManiVault, create **Knowledge Assistant** through View, select the package and executable if auto-detection fails.
5. Verify general questions, repository summaries, end-user UI queries with and without a guide, and source citations.
6. Test Stop/New chat/Export, resize, themes and settings persistence.
7. Fully disconnect the network and repeat tests to confirm runtime remains offline.
8. Check model licenses and rights to redistribute every indexed document.

## Known limitations

- The default worker keeps the model and reusable KV prefix in a local child process. Typing pauses trigger coalesced context preparation. Only exact token prefixes survive question/source/history changes. Pause/Resume suspends the active request; Stop cooperatively cancels it while retaining the loaded model, with a timed hard-kill fallback for unresponsive workers. Legacy externally selected llama-completion uses one process per question. See RUNTIME_PROTOCOL.md.
- Qt's SQL SQLite driver must be shipped with the matching Qt runtime. Most ManiVault developer bundles include it, but installers should verify this.
- Repo-name matching and UI-guide detection use transparent heuristics rather than a semantic embedding model; test against the real project corpus.
- Long answers are bounded by configured token limits; hallucinations remain possible.
- Windows x64 compilation and installation were verified against ManiVault 1.5 / Qt 6.10.3. Other native SDKs and full host UI interactions still require validation.

## UI architecture and checks

`ManiVaultControls` creates action-owned widgets in production. The plugin always defines `ASSISTANT_MANIVAULT_ACTIONS`; standalone layout tests use the adapter's plain Qt implementation because SDK action labels need the host's initialized theme and project managers. Test results for that adapter do not establish complete host integration.

`ChatUi` owns palettes, painter icons, Markdown bubbles, optional group animations and native charts. Preserve the original source under `assistantMarkdown`: `markdown` is an existing Qt property and round-tripping it loses formatting. `ChatStore` keeps compressed conversation JSON in a separate transactional SQLite database; messages are rendered in batches of 80 and persistence occurs at conversation changes, never per token. Stream redraws are coalesced. A wheel-up gesture suspends tail following, including when the pointer is over a message.

Run optional layout regressions with the Qt Test component and a local Qt Tooltip Kit checkout:

```sh
cmake -S tests/ui -B build/ui -DQt6_DIR=/path/to/Qt/lib/cmake/Qt6 -DTOOLTIP_SOURCE=/path/to/qt-tooltip-kit
cmake --build build/ui --config RelWithDebInfo
# Run assistant-ui-test /absolute/path/to/screenshots with Qt libraries on PATH.
```

The harness isolates history and settings in a temporary directory using `ASSISTANT_DATA_DIR` and `ASSISTANT_SETTINGS_FILE`. It checks eight explicit palettes, table preservation across theme changes, nonmodal settings at wide/narrow sizes, Shift+Enter, stream/completion scroll anchoring and branching. On Windows it explicitly loads Segoe UI for offscreen screenshots; other platforms use their Qt font fallback. The native real-model test additionally exercises startup queuing, changed/paraphrased tutorial queries, prepared latency, paused stream stability, resume, stop and retaining the same worker PID.

Further regressions cover edit/cancel/resubmit without modifying the source conversation, draft restoration, command filtering/execution, preference search, refresh intervals, nested question filtering, per-user AppDataLocation, installation-ID stability/isolation, transactional read-only history import and non-resurrection after deletion. `ChatStore` follows the `QStandardPaths::writableLocation(AppDataLocation)` convention used by core's `util::StandardPaths::getDownloadsDirectory`, with a plugin-owned child and a SHA-256 installation namespace. It does not modify core or change application/organization identity. No shared/system fallback is used if the writable user location is unavailable. History and INI preferences stay inside the same scoped folder. Legacy import is explicit: no global history scan or automatic cross-installation merge is performed. The source is read-only; failed imports roll back and leave current data and the source unchanged.

Production progress also creates a cancellable native ManiVault background task. Redraw cadence is independent of model generation speed. `ChatTextBrowser` skips unchanged Markdown and retains style/font state between streaming updates. Persisted chats are read on demand rather than duplicated in the view's memory map; the map retains only unsaved chats or failed saves.

Chat grouping uses a small `chat_groups` table with a foreign key to the existing conversation table. Older history databases remain readable without rewriting their compressed message payloads. JSON exports include groups and manual-edit flags; SQLite imports preserve group metadata only for newly imported conversations. Concurrent stale edits fork a copy with its group intact. The UI tests cover repeated saves, grouped import, filtering, inherited groups, edits across navigation, and rename-dialog races.

`AssistantControls::select` updates native `OptionAction` models, rather than only changing the presentation widget. Core's combo boxes use activation-only user signals and a QStringListModel; streaming intervals are mapped from the selection index rather than unsupported custom item roles. Preset changes batch persistence/redraws, and assistant shortcuts use WidgetWithChildrenShortcut so other views are unaffected.

The optional **real SDK control test** runs without a fully initialized host and checks OptionAction, TriggerAction, IntegralAction, DecimalAction and StringAction widgets:

```sh
cmake -S tests/native-actions -B build/native-actions -DManiVault_DIR=/path/to/sdk/cmake/mv -DQt6_DIR=/path/to/Qt/lib/cmake/Qt6
cmake --build build/native-actions --config RelWithDebInfo
# Run assistant-native-actions with the host and matching Qt libraries on PATH.
```

The real-model smoke executable also supports a corpus-neutral library check:

```sh
assistant-smoke --library /absolute/path/to/release /absolute/path/to/suggested-questions.json
```

This checks that prompts retrieve excerpts, not that every generated answer is correct. The default two-argument generation test uses the bundled EvoViewer example corpus.
