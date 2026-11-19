#include "ggml-backend.h"
#include "json.hpp"
#include "llama.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <clocale>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Json = nlohmann::json;
namespace {
bool hasGpu() {
    for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
        const auto type = ggml_backend_dev_type(ggml_backend_dev_get(i));
        if (type == GGML_BACKEND_DEVICE_TYPE_GPU || type == GGML_BACKEND_DEVICE_TYPE_IGPU)
            return true;
    }
    return false;
}

std::string deviceDescription() {
    for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
        const auto device = ggml_backend_dev_get(i);
        const auto type = ggml_backend_dev_type(device);
        if (type == GGML_BACKEND_DEVICE_TYPE_GPU || type == GGML_BACKEND_DEVICE_TYPE_IGPU)
            return std::string("GPU · ") + ggml_backend_dev_description(device);
    }
    return "CPU";
}

void send(const Json &event) {
    static std::mutex outputMutex;
    std::lock_guard<std::mutex> lock(outputMutex);
    std::cout << event.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n' << std::flush;
}

struct Control {
    std::atomic<int64_t> active{0};
    std::atomic<bool> paused{false}, cancelled{false};
    std::mutex mutex;
    std::condition_variable changed;
    bool proceed(int64_t id) {
        if (active != id)
            return true;
        if (cancelled)
            return false;
        if (paused) {
            send({{"type", "paused"}, {"id", id}});
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [&] { return !paused || cancelled || active != id; });
            if (cancelled)
                return false;
            send({{"type", "resumed"}, {"id", id}});
        }
        return !cancelled;
    }
};

class Engine {
    using Model = std::unique_ptr<llama_model, decltype(&llama_model_free)>;
    using Context = std::unique_ptr<llama_context, decltype(&llama_free)>;
    Model model_{nullptr, llama_model_free};
    Context context_{nullptr, llama_free};
    std::vector<llama_token> evaluated_;

    std::vector<llama_token> tokenize(const std::string &text) {
        const auto *vocab = llama_model_get_vocab(model_.get());
        const int count = -llama_tokenize(vocab, text.data(), int(text.size()), nullptr, 0, true, true);
        if (count <= 0)
            throw std::runtime_error("The prompt is empty or cannot be tokenized.");
        std::vector<llama_token> tokens(count);
        if (llama_tokenize(vocab, text.data(), int(text.size()), tokens.data(), count, true, true) != count)
            throw std::runtime_error("Prompt tokenization failed.");
        return tokens;
    }

    void decode(const llama_token *tokens, int count, int position) {
        auto *batch = llama_batch_ext_init(context_.get());
        for (int i = 0; i < count; ++i) {
            const int index = llama_batch_ext_add_token(batch, 0, tokens[i]);
            const llama_pos pos = position + i;
            llama_batch_ext_set_pos(batch, index, &pos);
        }
        llama_batch_ext_set_output_logits(batch, count - 1, true);
        const int result = llama_process(context_.get(), LLAMA_PROCESS_TYPE_DECODE, batch);
        llama_batch_ext_free(batch);
        if (result != 0) {
            evaluated_.clear();
            llama_memory_clear(llama_get_memory(context_.get()), true);
            throw std::runtime_error("Local model evaluation failed.");
        }
    }

  public:
    explicit Engine(const char *path) {
        auto parameters = llama_model_default_params();
        parameters.use_extra_bufts = false;
        parameters.n_gpu_layers = -1;
        parameters.progress_callback = [](float progress, void *) {
            send({{"type", "progress"},
                  {"id", 0},
                  {"stage", "loading"},
                  {"current", int(progress * 100)},
                  {"total", 100}});
            return true;
        };
        model_.reset(llama_model_load_from_file(path, parameters));
        if (!model_)
            throw std::runtime_error("Cannot load the selected GGUF model.");
        configure(4096);
        // Touch model pages before announcing readiness, rather than on Send.
        const auto tokens = tokenize("Hello");
        decode(tokens.data(), 1, 0);
        llama_memory_clear(llama_get_memory(context_.get()), true);
    }

    void configure(int contextSize) {
        contextSize = std::clamp(contextSize, 1024, 8192);
        if (context_ && int(llama_n_ctx(context_.get())) == contextSize)
            return;
        context_.reset();
        evaluated_.clear();
        auto parameters = llama_context_default_params();
        parameters.n_ctx = contextSize;
        parameters.n_batch = hasGpu() ? 512 : 256;
        parameters.n_ubatch = parameters.n_batch;
        parameters.offload_kqv = true;
        parameters.op_offload = true;
        parameters.n_threads = std::clamp(int(std::thread::hardware_concurrency() / 2), 1, 8);
        parameters.n_threads_batch = parameters.n_threads;
        context_.reset(llama_init_from_model(model_.get(), parameters));
        if (!context_)
            throw std::runtime_error("Cannot allocate the selected context size.");
    }

    void request(const Json &request, Control &control) {
        const auto started = std::chrono::steady_clock::now();
        const int64_t id = request.at("id").get<int64_t>();
        if (!control.proceed(id)) {
            send({{"type", "done"}, {"id", id}, {"error", ""}, {"cancelled", true}});
            return;
        }
        configure(request.value("context_tokens", 4096));
        const auto tokens = tokenize(request.at("prompt").get<std::string>());
        const bool prepare = request.value("type", "ask") == "prepare";
        const int outputLimit = prepare ? 0 : std::clamp(request.value("max_tokens", 256), 1, 1024);
        if (tokens.size() + outputLimit > llama_n_ctx(context_.get()))
            throw std::runtime_error("The question and sources exceed the context size. Reduce "
                                     "sources/history or increase context tokens.");

        // Retain only an exact matching token prefix. Changed sources, history,
        // questions or settings can never reuse stale suffix logits or KV state.
        size_t common = 0;
        while (common < tokens.size() && common < evaluated_.size() && tokens[common] == evaluated_[common])
            ++common;
        common = std::min(common, tokens.size() - 1); // Recompute final logits.
        if (!llama_memory_seq_rm(llama_get_memory(context_.get()), 0, llama_pos(common), -1)) {
            llama_memory_clear(llama_get_memory(context_.get()), true);
            common = 0;
        }
        evaluated_.resize(common);
        send({{"type", "progress"},
              {"id", id},
              {"stage", "context"},
              {"current", common},
              {"total", tokens.size()}});
        for (size_t position = common; position < tokens.size();) {
            if (!control.proceed(id)) {
                send({{"type", "done"}, {"id", id}, {"error", ""}, {"cancelled", true}});
                return;
            }
            const int count = int(std::min(size_t(llama_n_batch(context_.get())), tokens.size() - position));
            decode(tokens.data() + position, count, int(position));
            evaluated_.insert(evaluated_.end(), tokens.begin() + position, tokens.begin() + position + count);
            position += count;
            send({{"type", "progress"},
                  {"id", id},
                  {"stage", "context"},
                  {"current", position},
                  {"total", tokens.size()}});
        }
        if (prepare) {
            send({{"type", "prepared"}, {"id", id}});
            return;
        }

        const auto generationStarted = std::chrono::steady_clock::now();

        using Sampler = std::unique_ptr<llama_sampler, decltype(&llama_sampler_free)>;
        Sampler sampler(llama_sampler_chain_init(llama_sampler_chain_default_params()), llama_sampler_free);
        const auto *vocab = llama_model_get_vocab(model_.get());
        llama_sampler_chain_add(
            sampler.get(), llama_sampler_init_penalties(
                               llama_vocab_n_tokens(vocab), 64,
                               std::clamp(request.value("repeat_penalty", 1.1f), 1.0f, 1.5f), 0.0f, 0.0f));
        const float temperature = std::clamp(request.value("temperature", 0.2f), 0.0f, 1.0f);
        if (temperature <= 0)
            llama_sampler_chain_add(sampler.get(), llama_sampler_init_greedy());
        else {
            llama_sampler_chain_add(sampler.get(), llama_sampler_init_top_k(40));
            llama_sampler_chain_add(sampler.get(), llama_sampler_init_top_p(0.9f, 1));
            llama_sampler_chain_add(sampler.get(), llama_sampler_init_temp(temperature));
            llama_sampler_chain_add(sampler.get(), llama_sampler_init_dist(42));
        }
        std::string answer;
        int generated = 0;
        for (int i = 0; i < outputLimit; ++i) {
            if (!control.proceed(id))
                break;
            const auto token = llama_sampler_sample(sampler.get(), context_.get(), -1);
            if (llama_vocab_is_eog(vocab, token))
                break;
            std::vector<char> piece(256);
            int count = llama_token_to_piece(vocab, token, piece.data(), int(piece.size()), 0, false);
            if (count < 0) {
                piece.resize(-count);
                count = llama_token_to_piece(vocab, token, piece.data(), int(piece.size()), 0, false);
            }
            if (count < 0)
                throw std::runtime_error("Cannot decode the generated text.");
            answer.append(piece.data(), count);
            ++generated;
            send({{"type", "text"}, {"id", id}, {"text", answer}});
            if (generated == 1 || generated % 4 == 0)
                send({{"type", "progress"},
                      {"id", id},
                      {"stage", "generating"},
                      {"current", generated},
                      {"total", outputLimit}});
            if (i + 1 < outputLimit) {
                decode(&token, 1, int(evaluated_.size()));
                evaluated_.push_back(token);
            }
        }
        const auto finished = std::chrono::steady_clock::now();
        send({{"type", "done"},
              {"id", id},
              {"error", ""},
              {"cancelled", control.cancelled && control.active == id},
              {"generated_tokens", generated},
              {"prompt_tokens", tokens.size()},
              {"reused_tokens", common},
              {"prefill_ms", std::chrono::duration<double, std::milli>(generationStarted - started).count()},
              {"generation_ms",
               std::chrono::duration<double, std::milli>(finished - generationStarted).count()}});
    }
};
} // namespace

int run(const std::string &modelPath) {
    std::setlocale(LC_NUMERIC, "C");
    llama_backend_init();
    ggml_backend_load_all();
    try {
#ifdef ASSISTANT_REQUIRE_GPU
        if (!hasGpu())
            throw std::runtime_error("No compatible GPU is available. Check the GPU driver or build with "
                                     "ASSISTANT_GPU_BACKEND=CPU for CPU fallback.");
#endif
        Engine engine(modelPath.c_str());
        send({{"type", "ready"}, {"backend", deviceDescription()}});
        Control control;
        std::mutex queueMutex;
        std::condition_variable queued;
        std::deque<Json> requests;
        bool closed = false;
        std::thread reader([&] {
            std::string line;
            while (std::getline(std::cin, line)) {
                try {
                    auto request = Json::parse(line);
                    const auto type = request.value("type", "ask");
                    const int64_t id = request.value("id", int64_t(0));
                    if (type == "pause" || type == "resume" || type == "stop") {
                        std::lock_guard<std::mutex> lock(control.mutex);
                        if (control.active == id) {
                            control.paused = type == "pause";
                            if (type == "stop")
                                control.cancelled = true;
                            control.changed.notify_all();
                        }
                    } else {
                        if (type == "ask") {
                            std::lock_guard<std::mutex> lock(control.mutex);
                            control.active = id;
                            control.paused = false;
                            control.cancelled = false;
                        }
                        {
                            std::lock_guard<std::mutex> lock(queueMutex);
                            requests.push_back(std::move(request));
                        }
                        queued.notify_one();
                    }
                } catch (const std::exception &error) {
                    send({{"type", "done"}, {"id", 0}, {"error", error.what()}});
                }
            }
            {
                std::lock_guard<std::mutex> lock(control.mutex);
                control.cancelled = true;
                control.paused = false;
            }
            control.changed.notify_all();
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                closed = true;
            }
            queued.notify_one();
        });
        for (;;) {
            Json request;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                queued.wait(lock, [&] { return closed || !requests.empty(); });
                if (closed && requests.empty())
                    break;
                request = std::move(requests.front());
                requests.pop_front();
            }
            try {
                engine.request(request, control);
            } catch (const std::exception &error) {
                const auto object = request.is_object() ? request : Json::object();
                send({{"type", object.value("type", "ask") == "prepare" ? "prepared" : "done"},
                      {"id", object.value("id", int64_t(0))},
                      {"error", error.what()}});
            }
        }
        reader.join();
    } catch (const std::exception &error) {
        send({{"type", "fatal"}, {"error", error.what()}});
        llama_backend_free();
        return 1;
    }
    llama_backend_free();
    return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    if (argc != 3 || std::wstring(argv[1]) != L"--model")
        return 2;
    return run(std::filesystem::path(argv[2]).u8string());
}
#else
int main(int argc, char **argv) {
    if (argc != 3 || std::string(argv[1]) != "--model")
        return 2;
    return run(argv[2]);
}
#endif
