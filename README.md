# Knowledge Assistant for ManiVault Studio

A native Qt View plugin that answers questions using a local GGUF model and a read-only SQLite knowledge index. Inference runs through a bundled llama.cpp executable. No Python, API key, server or internet connection is required at runtime.

The workspace includes searchable commands and preferences, reversible question editing/resubmission, chat drafts, native ManiVault tasks, progress diagnostics and configurable streaming refresh. Saved chats and preferences live in `<per-user AppDataLocation>/ManiVaultChatAssistant/instances/<installation-id>/`. The installation ID is a stable hash of the canonical host application directory. Previous history can be imported explicitly without changing the source file. The bundled, replaceable question JSON contains nested plugin and viewer/project categories, including PathwayFinder, Basal Ganglia Drill-in and Patch-seq. Corpus names are confined to example data; the UI and inference code remain general.

## Build inside a ManiVault development bundle

Add this plugin after the core, following the same pattern as EvoViewerSetupPlugin:

```cmake
add_subdirectory(core)
add_subdirectory(chat-assistant-plugin)
```

The plugin reuses the existing `ManiVault::Core` target and ManiVault CMake helpers. It does not require an installed ManiVault package when core is already part of the build. Build `ChatAssistantPlugin` with the same configuration as your host (for example RelWithDebInfo). In a bundle, automatic installation defaults to ON and places the plugin and assets under `${ManiVault_INSTALL_DIR}/<configuration>/Plugins`.

Visual Studio groups the plugin under `ChatAssistantPlugin`, with its required libraries under `Dependencies` and the resident inference worker under `Runtime`. Unused upstream llama.cpp tools, benchmarks and multimodal executables are not added. These remain separate CMake build targets because the plugin launches the runtime as a child process; they are packaged together under the plugin's installation directory.

```sh
cmake -S source -B build
cmake --build build --config RelWithDebInfo --target ChatAssistantPlugin --parallel 2
```

## Standalone SDK build

Use the same compiler, CPU architecture and Qt installation as ManiVault. Requirements: CMake 3.22+, C/C++ compiler, ManiVault 1.5 development SDK, Qt 6.8+ Core, Widgets, Network and Sql. Qt's QSQLITE driver must accompany the installed application. Network is needed by the SDK headers; the assistant itself performs no network requests at runtime.

```sh
cmake -S . -B build \
  -DManiVault_DIR="/path/to/ManiVault/install/cmake/mv" \
  -DQt6_DIR="/path/to/Qt/lib/cmake/Qt6"
cmake --build build --config RelWithDebInfo --target ChatAssistantPlugin --parallel 2
cmake --install build --config RelWithDebInfo --component PLUGINS --prefix "/path/to/ManiVault/install/RelWithDebInfo"
```

On PowerShell use a single line or PowerShell backticks instead of shell backslashes. For single-configuration generators also set `-DCMAKE_BUILD_TYPE=RelWithDebInfo`. The install prefix must be the directory containing the host application. If configuration reports `ManiVault::Core is unavailable`, supply the SDK directory containing `ManiVaultConfig.cmake`, or build inside the bundle after core.

## Model download and replacement

The default package is [tutorials-papers-assistant.zip](https://sec.lumc.nl/mtg-viewer/MV_data/CytosploreViewer/TutorialModels/tutorials-papers-assistant.zip), approximately 920 MiB. It contains the trained 1.5B Qwen model, tutorial/paper index, reports and license information. The ZIP SHA256 is:

```text
d6cdfd84ae7ae47b693f48ac8e1612fe3dc49c957675b19c670a7f28e643cc34
```

CMake downloads it once, verifies the archive and included file checksums, and stages its contents. Internet access is needed during initial configuration. Allow several GB of build disk space and enough RAM for inference.

To use your own [Local Knowledge Trainer](https://github.com/sbvis/local-knowledge-trainer) release, configure a fresh build with `-DASSISTANT_PACKAGE_DIR=/path/to/release`. For an existing cache also clear the prior URL and digest with `-DASSISTANT_PACKAGE_URL= -DASSISTANT_PACKAGE_SHA256=`. Alternatively set both `ASSISTANT_PACKAGE_URL` and `ASSISTANT_PACKAGE_SHA256` to your own hosted ZIP and digest. ZIP files may contain a `release/` directory or files directly at the root. Keep `assistant.gguf`, `knowledge.db`, metadata and licenses together. Complete `model-shards/` exports are also supported.

The integration is generic: URLs and local package paths are configurable, and C++ contains no tutorial-specific answers. Do not commit model files or indexed private data to Git.

## Platforms and runtime

Build separately for Windows, Linux, Apple Silicon macOS and Intel macOS, using matching ManiVault/Qt SDKs. The same model ZIP works on each platform. `ASSISTANT_GPU_BACKEND=AUTO` selects Metal on macOS (Intel and Apple Silicon), Vulkan on Windows/Linux when its build tools are available, and CPU otherwise. Explicit `METAL` or `VULKAN` builds require their SDKs and a compatible runtime GPU rather than silently using CPU. `CPU` selects CPU fallback. A macOS binary must match the host/SDK architecture; this project does not automatically create a universal application.

The x86 CPU runtime enables AVX2/FMA acceleration by default. For older CPUs without AVX2, configure `-DASSISTANT_ENABLE_AVX2=OFF` (inference will be slower). Defaults use two source excerpts and a 256-token answer limit; increase these in Settings for broader evidence or longer answers. The bundled worker loads the model once when the view opens and keeps it in memory. It reuses only exact matching prompt-token prefixes and prepares source context during typing pauses. The latest pending preparation replaces older pending input; submitted questions take priority. HTML markup is removed from document excerpts. Pasted questions sent immediately still need prompt processing, and complete answers take generation time.

The bundled runtime uses pinned llama.cpp revision `2cbfc3cf34f94c8ada67eb8766efe7374a3d2db6` from a checksum-verified source archive. `ASSISTANT_LLAMA_SOURCE_DIR` accepts a local source tree for offline builds. `ASSISTANT_BUILD_LLAMA=OFF` allows selecting an existing compatible executable in Settings.

Installed layout:

```text
Plugins/
  ChatAssistantPlugin_<SDK version suffix>.dll (or platform shared library)
  ChatAssistantPlugin/
    runtime/assistant-runtime[.exe]
    data/assistant.gguf
    data/knowledge.db
    data/<reports and licenses>
```

Create a **Knowledge Assistant** view in ManiVault. Installed paths are detected automatically. Settings can select another package folder or executable. The UI provides saved multi-chat navigation, branching and linked context, per-message actions, Pause/Resume/Stop, task progress, grouped question-file import, Markdown/JSON export, native charts, nine themes, rich tooltips and native ManiVault actions. Preferences use a side drawer or a separate nonmodal window. See [the user guide](docs/USER_GUIDE.md). The model stays loaded until the view closes or paths change. The ready status identifies the detected compute device. GPU builds request all model layers and the attention cache on the GPU; streaming redraws are coalesced to keep the interface responsive. Pause/Resume suspends token generation and Stop cancels the current request while retaining the model; an unresponsive worker has a bounded hard-stop fallback. A compatible externally selected `llama-completion` executable remains supported as a slower one-shot fallback.

## GPU configuration

GPU build presets (pass the matching `ManiVault_DIR` and `Qt6_DIR` as described above):

| Native platform | Preset | GPU API | Build prerequisites |
| --- | --- | --- | --- |
| Windows | `windows-gpu` | Vulkan | Vulkan SDK with glslc and SPIRV-Headers |
| Linux | `linux-gpu` | Vulkan | Vulkan development loader/headers, glslc and SPIRV-Headers |
| Intel macOS | `macos-intel` | Metal | Xcode command-line/Metal build tools and x86_64 SDKs |
| Apple Silicon macOS | `macos-arm64` | Metal | Xcode command-line/Metal build tools and arm64 SDKs |
| Any platform | `cpu` | CPU fallback | Matching compiler and ManiVault/Qt SDKs |

For example, `cmake --preset windows-gpu -DManiVault_DIR=... -DQt6_DIR=...`, then `cmake --build --preset windows-gpu`. For an existing bundle set `-DASSISTANT_GPU_BACKEND=VULKAN`, `METAL`, `CPU` or `AUTO` in its CMake cache. On Windows/Linux, set `VULKAN_SDK` or provide the corresponding CMake package paths if SDK discovery needs assistance. The Vulkan shader tools and SDK are build dependencies; users of the installed plugin need a compatible GPU driver/Vulkan loader. Metal uses the macOS frameworks. Hardware support and driver capabilities still determine which GPU operations are available. Native Linux/macOS GPU builds require validation on their corresponding SDKs and hardware.

## Validation

Windows x64 compilation and installation have been verified locally with ManiVault 1.5 and Qt 6.10.3. Package acquisition tests exercise actual CMake staging, checksum failures, replacement and sharded releases. Full host GUI behavior and native macOS/Linux builds require validation with their corresponding SDKs; package CI alone does not establish those results.

```sh
python -m unittest discover -s tests -v
```

An optional native retrieval/inference smoke test uses a real release and executable:

```sh
cmake -S tests/native -B build/smoke -DQt6_DIR=/path/to/Qt/lib/cmake/Qt6
cmake --build build/smoke --config Release
# Run the generated assistant-smoke executable with these two arguments:
# /path/to/release /path/to/assistant-runtime
```

Answers are grounded in retrieved excerpts, but a small language model can still make factual mistakes. Long documents are excerpted and retrieval is lexical. Test your own representative questions before distributing a replacement model. Plugin sources use MIT; model, source documents and llama.cpp retain their respective licenses.
