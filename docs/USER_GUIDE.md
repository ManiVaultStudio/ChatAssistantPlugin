# Using Knowledge Assistant

Open **Knowledge Assistant** from ManiVault's View menu. The header identifies the running compute device. Initial model loading takes longer than a prepared follow-up question.

## Conversation workspace

- **Enter** sends; **Shift+Enter** adds a line. Ctrl/Cmd+Enter also sends. Preferences can change Enter to insert a line instead.
- **Pause / Resume** suspends generation between compute steps; **Stop** keeps the resident model loaded and preserves the answer received so far. These controls require the bundled `assistant-runtime`.
- Scroll upward to read without being pulled back down. **Jump to latest** resumes following. Streaming redraws are batched at a configurable 16–100 ms (32 ms by default).
- The left sidebar contains searchable saved chats and a grouped question library. Hide it to maximize reading space.
- Use the group filter to show a project group, pinned chats or ungrouped chats. Choose Recent, Title or Group ordering. Pinned chats remain first. Right-click **Move to group** to choose an existing group or type a new name; **Remove from group** leaves the conversation intact. New chats inherit the selected group. **F2** renames the selected chat.
- Right-click a chat in the left Chats tab for Open, Rename, Pin, Branch, Copy, Export and Delete. Delete also works from the keyboard, with confirmation. Deleting a different chat preserves the active response. Right-click a message or chart for message actions, selected-text copy and chart CSV/PNG export where available.
- Each message has Copy, Branch, Bookmark and a menu for deletion, trimming or regeneration. Branching copies the conversation up to that message. Editing a question creates a branch.
- **Edit and resubmit** loads an earlier question into the composer. **Cancel edit** restores your unsent draft; **Submit edit** branches at that question and sends the revision. The original conversation remains available. Unsent drafts survive switching chats in the current view until it closes. Code answers also offer **Copy code block**.
- Right-click **Edit saved message** to correct either a question or an assistant reply in a nonmodal editor. Saved edits carry an **Edited** label. Existing replies are retained; this action does not regenerate a response. Manually edited assistant replies no longer carry automatically supplied source excerpts. Cancel leaves the message unchanged. Rename and grouping actions on another chat do not switch away from the active response.
- **Ctrl/Cmd+K** opens searchable commands; **Ctrl/Cmd+,** toggles preferences; **Ctrl/Cmd+Shift+Space** pauses/resumes. The command palette is nonmodal. Preferences have their own search field.
- The conversation menu supports rename, pin, import/export, deletion and explicitly including context from another chat. Chats otherwise remain independent. Each plugin view has its own conversation and inference process; opening many views duplicates model memory.
- Sources expand below the answer. Open an excerpt to inspect the supporting text. Documents and generated answers may contain errors; verify important claims against the source.

## Preferences

Preferences open beside the chat at wide sizes and in a separate nonmodal window at narrow sizes. They never replace the conversation. Close the window or toggle Preferences to dismiss it.

Choose System, Light, Dark, Midnight, Nord, Graphite, Sepia, Rose or High contrast. Adjust accent, text size, reading width, spacing, timestamps and optional group animations. Native ManiVault `OptionsAction` controls which message tools are visible; conversation commands use `TriggerAction`, preferences use `OptionAction`, `ToggleAction`, numeric/String actions and `GroupAction`.

Fast, Balanced and Detailed presets trade response length and retrieved context for latency. Custom answer format, token limits, source count, history turns, temperature, repetition penalty and response instructions are available. Context preparation while typing reduces latency when it completes before Send. Full GPU offload does not mean every GPU must report 100% utilization, or that every answer is instantaneous.

History and preferences are stored under **`<per-user AppDataLocation>/ManiVaultChatAssistant/instances/<installation-id>/`** in `chats.sqlite3` and `preferences.ini`. This follows core's Qt writable application-data convention: Windows uses the user's roaming application data, macOS uses the user's Application Support directory, and Linux follows the user's XDG application-data directory. The installation ID is a stable hash of the canonical ManiVault application directory, so different users and different installations keep independent histories and settings. Multiple windows of the same installation can share that installation's history. Updating in the same installation path keeps the ID; moving to another path creates a new instance scope.

**Open conversation folder** opens the exact location. **Import previous history** explicitly reads a selected old assistant database, transactionally retaining current chats and leaving the original unchanged. Unscoped history is never imported automatically, since it may belong to another installation. Core directories, global settings, system-wide locations and model data are not modified. Disabling saved history stops future persistence; it does not erase existing chats. Use **Delete all saved conversations** to erase this installation's history, or export first. Concurrent edits to the same stored chat create a copy rather than overwrite another view's changes.

The task strip explains loading, source search, context processing and generation. Hover over progress and timing for stage counts, cached prompt tokens and generation metrics. Native ManiVault background tasks track the active request and expose cancellation. **Copy diagnostics** copies device, paths and timing without conversation text. Reduced motion disables indeterminate progress animation as well as optional group transitions.

## Suggested questions

Import a UTF-8 text file (one question per nonempty line) or JSON:

```json
{"groups":[{"title":"Getting started","questions":[{"label":"Explore my documents","prompt":"Summarize the main topics in these documents."}]}]}
```

Groups and subgroups collapse independently and are searchable. A group may contain both `questions` and another `groups` array. Libraries support up to six levels, 100 groups and 300 questions, in a file smaller than 1 MB. Reload after editing; export to save custom questions.

The bundled replaceable example library contains 160 questions, with plugin categories such as Data Hierarchy and Data Properties; project categories for EvoViewer, SimianViewer, Gradient Surfer, ATAC Viewer, PathwayFinder, Basal Ganglia Drill-in and Cytosplore Patch-seq; and multi-plugin hyperspectral workflows. Viewer subcategories cover inputs, linked-plugin workflows and interpretation. These are prompts, not guaranteed answers: missing evidence must still be reported. All corpus-specific names live in the example JSON, not in inference logic. Replace the file for a different corpus; doing so does not retrain the model or alter its index.

## Charts

Answers can include a native bar, line or scatter chart using an explicit `chart` JSON code block with `type`, `title`, optional `labels`, and numeric `values` (or scatter `points`). Charts can copy CSV and export PNG. The plugin never invents numeric data to generate a chart. Unstructured charts and external scripts are not executed.

## Performance and troubleshooting

Use a GPU build and verify the device in the header. Windows/Linux use Vulkan; Intel and Apple Silicon macOS use Metal. Required GPU builds fail visibly when no usable device is available. AUTO may select CPU at configuration if GPU SDKs are unavailable. Keep drivers current and match plugin/host architecture and Qt.

Use Fast for short answers, keep the view open to retain the model, and avoid unnecessarily long history/source excerpts. Model-size changes require a compatible replacement package. Changing appearance does not retrain or reload the model. See README for build requirements and native platform validation limits.
