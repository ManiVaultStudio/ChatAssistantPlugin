# Resident local runtime

`assistant-runtime --model /path/to/assistant.gguf` loads a causal GGUF model once and communicates through UTF-8 JSON lines on stdin/stdout. Diagnostics go to stderr. The process opens no network endpoint and saves no prompts, answers or KV state to disk.

After loading and touching the model pages it emits `{"type":"ready"}`. Each request includes a unique integer `id`, `type` (`prepare` or `ask`), `prompt`, `context_tokens` (1024–8192), `max_tokens` (1–1024), and `temperature` (0–1).

`prepare` evaluates the prompt without generating an answer, then emits `{"type":"prepared","id":1}`. `ask` emits `{"type":"text","id":2,"text":"complete answer so far"}` as tokens arrive, then `{"type":"done","id":2,"error":""}`. Request errors use the corresponding completion event with a nonempty `error`; startup errors emit `{"type":"fatal","error":"..."}` and exit. EOF releases the model and exits. Control requests `pause`, `resume` and `stop` target the active request ID. Pause emits `paused`, resume emits `resumed`, and stop emits `done` with `cancelled: true`; the loaded model remains resident. Progress and final token/prefill/generation metrics accompany responses.

Only a matching prefix of actual evaluated token IDs is retained. A changed suffix is removed from KV memory before evaluation, and final prompt logits are always recomputed. Context-size changes recreate the context and invalidate the prefix. Generated output is not silently added to future questions: the UI explicitly supplies the chosen conversation history.

The Qt controller allows one preparation in flight, coalesces further typing into a single latest pending request, and prioritizes Send over pending preparation. Request IDs reject stale output. The 300 ms typing debounce changes neither the submitted question nor the source selection. Preparation reduces latency when it completes before Send; it cannot make an unprepared prompt or a full multi-token answer instantaneous.

Use the native smoke test to check queued startup requests, changed questions, reuse of the same process, pause/resume, cancellation, model retention and prepared first-text latency with a real release. Timing depends on the model, hardware and prompt size.
