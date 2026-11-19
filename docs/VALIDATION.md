# Validation and release scope

Local validation uses Windows x64, Visual Studio 2026, ManiVault 1.5, Qt 6.10.3 and the bundled model package. GPU inference uses Vulkan on an NVIDIA GTX 1080. Results depend on hardware, model, prompt and preparation state.

The release checks include:

- Production plugin and resident runtime compilation and staged installation.
- Package checksums, archive staging/replacement, schema handling and platform backend-selection tests.
- Eight explicit palettes, Markdown table preservation, nonmodal preferences and responsive layouts.
- Scoped shortcuts, streaming/completion scroll stability, pause/resume/stop and worker retention.
- Chat rename, pin/group filters and sorting, branching, drafts, Delete and context menus.
- Direct saved-message editing, edit/resubmit, imported groups, concurrent-view conflicts and rename races.
- Per-user/per-installation storage isolation and read-only legacy import.
- Real ManiVault action widgets and programmatic model synchronization.
- Source retrieval for the shipped suggested-question library against the bundled search index.

The default example-model smoke test verifies the documented Shift-click interaction for EvoViewer with paraphrased questions. Prepared first text has measured below 100 ms locally; initial model loading and unprepared prompts take longer. These timings do not guarantee instant responses on other hardware.

Native macOS (Intel/Apple Silicon) and Linux compilation, driver behavior and full host interaction require release testing on those platforms. The cross-platform package/backend CI is not a substitute for those builds. Full host UI inspection is a separate acceptance step; the standalone layout harness and native SDK control test cover different parts of the UI.

Model-generated answers can still be wrong. Excerpt retrieval coverage does not prove answer correctness. Test representative workflows for each replacement corpus, including missing-evidence behavior, before distributing a new model.
