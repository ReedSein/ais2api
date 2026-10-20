// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/browser.hpp"
#include "ais2api/flow.hpp"
#include "ais2api/management.hpp"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <algorithm>
#include <csignal>
#include <deque>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
namespace ais2api {
namespace {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = net::ip::tcp;
using namespace std::chrono_literals;
std::atomic<unsigned> live_connections{0};
struct ConnectionSlot {
    ~ConnectionSlot() { live_connections.fetch_sub(1); }
};
struct Work {
    Protocol protocol = Protocol::gemini;
    std::string model, principal, id = "chatcmpl-" + random_hex(12);
    bool stream = false, include_usage = false;
    std::size_t input_bytes = 0;
    json::object body;
    std::shared_ptr<EventPipe> pipe;
    std::shared_ptr<net::steady_timer> wakeup;
    Clock::time_point queued = Clock::now();
};
struct Task {
    std::function<void()> execute;
    std::function<void()> cancel;
};
class Service {
  public:
    explicit Service(Config config, std::unique_ptr<GenerationBackend> backend)
        : config_(std::move(config)), store_(config_.data_dir),
          gateway_(backend ? std::move(backend)
                           : std::make_unique<BrowserGateway>(config_, store_)),
          worker_([this](std::stop_token token) { work_loop(token); }) {}
    ~Service() { shutdown(); }
    void shutdown() {
        stop();
        worker_.request_stop();
        available_.notify_all();
        if (worker_.joinable())
            worker_.join();
        gateway_->close();
    }
    Config config() {
        std::lock_guard lock(config_mutex_);
        return config_;
    }
    Store &store() { return store_; }
    bool stopping() const { return stopping_; }
    void stop() {
        stopping_ = true;
        shutdown_.request_stop();
        std::lock_guard lock(queue_mutex_);
        if (active_)
            active_->cancel();
        for (auto &task : queue_)
            task.cancel();
        available_.notify_all();
    }
    bool enqueue(const std::shared_ptr<Work> &work) {
        auto cfg = config();
        std::lock_guard lock(queue_mutex_);
        if (stopping_ || queue_.size() >= cfg.limits.queue_size)
            return false;
        auto weak = std::weak_ptr<Work>(work);
        queue_.push_back({[this, weak] {
                              if (auto pending = weak.lock())
                                  generate(pending);
                          },
                          [weak] {
                              auto work = weak.lock();
                              if (!work)
                                  return;
                              work->pipe->cancel();
                              work->pipe->finish(Error(503, "stopping", "Service is stopping"));
                          }});
        available_.notify_one();
        return true;
    }
    json::array cached_models() {
        std::lock_guard lock(models_mutex_);
        auto out = models_;
        if (out.empty()) {
            auto cfg = config();
            for (auto &item : cfg.configured_models) {
                if (item.is_string())
                    out.push_back(json::object{{"name", "models/" + std::string(item.as_string())},
                                               {"source", "configured"}});
                else if (item.is_object()) {
                    auto o = item.as_object();
                    o["source"] = "configured";
                    out.push_back(std::move(o));
                }
            }
        }
        return out;
    }
    json::object control(const json::object &command) {
        auto action = string_or(command, "command");
        if (action == "status") {
            std::lock_guard lock(queue_mutex_);
            return {{"running", true},
                    {"active", bool(active_)},
                    {"queued", queue_.size()},
                    {"connections", live_connections.load()},
                    {"ready", store_.choose("readiness").has_value() &&
                                  fs::is_regular_file(config().browser_executable)},
                    {"verification", "not_a_generation_check"}};
        }
        if (action == "stop") {
            stop();
            return {{"status", "stopping"}};
        }
        if (action == "operation.status") {
            std::lock_guard lock(operations_mutex_);
            auto it = operations_.find(string_or(command, "id"));
            if (it == operations_.end())
                throw Error(404, "operation_missing", "Unknown or expired operation");
            return it->second.result;
        }
        if (action == "config.reload") {
            auto previous = config();
            auto next = Config::load(previous.config_file);
            if (next.bind != previous.bind || next.port != previous.port ||
                next.data_dir != previous.data_dir ||
                next.browser_executable != previous.browser_executable ||
                next.proxy != previous.proxy || next.headless != previous.headless ||
                next.limits.generation_seconds != previous.limits.generation_seconds ||
                next.limits.stream_idle_seconds != previous.limits.stream_idle_seconds ||
                next.limits.object_bytes != previous.limits.object_bytes ||
                next.limits.relay_bytes != previous.limits.relay_bytes)
                throw Error(
                    409, "restart_required",
                    "Browser, listener, storage and transport-limit changes require a restart");
            std::lock_guard lock(config_mutex_);
            config_ = std::move(next);
            return {{"status", "reloaded"}};
        }
        if (action == "models.list" || action == "accounts.probe")
            return maintenance(command);
        // Never expose these mutations through HTTP. This is owner-only IPC.
        auto cfg = config();
        return manage(store_, cfg, command);
    }

  private:
    struct Operation {
        json::object result;
        Clock::time_point created;
    };
    Config config_;
    std::mutex config_mutex_;
    Store store_;
    std::unique_ptr<GenerationBackend> gateway_;
    std::mutex queue_mutex_, models_mutex_, operations_mutex_;
    std::condition_variable available_;
    std::deque<Task> queue_;
    std::shared_ptr<EventPipe> active_;
    json::array models_;
    std::map<std::string, Operation> operations_;
    std::atomic<bool> stopping_{false};
    std::stop_source shutdown_;
    std::jthread worker_;
    void work_loop(std::stop_token token) {
        while (!token.stop_requested()) {
            Task task;
            {
                std::unique_lock lock(queue_mutex_);
                available_.wait(
                    lock, [&] { return token.stop_requested() || stopping_ || !queue_.empty(); });
                if (token.stop_requested() || stopping_)
                    break;
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            try {
                task.execute();
            } catch (...) {
                task.cancel();
            }
        }
    }
    json::object maintenance(const json::object &command) {
        auto cfg = config();
        auto id = random_hex(12);
        {
            std::lock_guard lock(operations_mutex_);
            for (auto i = operations_.begin(); i != operations_.end();)
                if (Clock::now() - i->second.created > 15min)
                    i = operations_.erase(i);
                else
                    ++i;
            if (operations_.size() >= 32)
                throw Error(429, "operations_full", "Too many retained control operations");
            operations_[id] = {{{"status", "queued"}, {"operation_id", id}}, Clock::now()};
        }
        auto finish = [this, id](json::object result) {
            std::lock_guard lock(operations_mutex_);
            auto it = operations_.find(id);
            if (it != operations_.end())
                it->second.result = std::move(result);
        };
        std::lock_guard lock(queue_mutex_);
        if (stopping_ || queue_.size() >= cfg.limits.queue_size) {
            std::lock_guard operation_lock(operations_mutex_);
            operations_.erase(id);
            throw Error(429, "queue_full", "Work queue is full");
        }
        queue_.push_back(
            {[this, command, finish] {
                 try {
                     json::object result;
                     gateway_->reconfigure(config());
                     if (string_or(command, "command") == "models.list") {
                         auto values = gateway_->models(shutdown_.get_token());
                         {
                             std::lock_guard guard(models_mutex_);
                             models_ = values;
                         }
                         result = {{"models", values}};
                     } else {
                         json::array results;
                         auto id = string_or(command, "id");
                         auto accounts = id.empty() ? store_.accounts()
                                                    : std::vector<Account>{store_.account(id)};
                         for (auto &a : accounts) {
                             check_cancel(shutdown_.get_token());
                             try {
                                 auto value = gateway_->probe(a, shutdown_.get_token());
                                 value["account_id"] = a.id;
                                 results.push_back(std::move(value));
                             } catch (const Error &e) {
                                 if (e.status == 401)
                                     store_.failure(a.id, "probe", "login_expired");
                                 if (!id.empty() || e.code == "cancelled")
                                     throw;
                                 results.push_back(
                                     json::object{{"account_id", a.id},
                                                  {"status", "failed"},
                                                  {"error", json::object{{"status", e.status},
                                                                         {"code", e.code},
                                                                         {"message", e.what()}}}});
                             }
                         }
                         result = {{"accounts", std::move(results)}};
                     }
                     finish({{"status", "done"}, {"result", std::move(result)}});
                 } catch (const Error &e) {
                     finish({{"status", "failed"},
                             {"error",
                              {{"status", e.status}, {"code", e.code}, {"message", e.what()}}}});
                 } catch (...) {
                     finish({{"status", "failed"},
                             {"error",
                              {{"status", 500},
                               {"code", "control_failed"},
                               {"message", "Control operation failed"}}}});
                 }
             },
             [finish] {
                 finish(
                     {{"status", "failed"},
                      {"error",
                       {{"status", 503}, {"code", "stopping"}, {"message", "Service stopped"}}}});
             }});
        available_.notify_one();
        return {{"operation_id", id}, {"status", "queued"}};
    }
    void prepare_media(json::object &body, const Config &cfg, std::stop_token token) {
        auto messages = body.if_contains("messages");
        if (!messages || !messages->is_array())
            return;
        auto size = json::serialize(body).size();
        if (size > cfg.limits.request_bytes)
            throw Error(413, "media_limit", "Normalized media request exceeds the request budget");
        for (auto &message : messages->as_array())
            if (message.is_object()) {
                auto content = message.as_object().if_contains("content");
                if (!content || !content->is_array())
                    continue;
                for (auto &part : content->as_array())
                    if (part.is_object() && string_or(part.as_object(), "type") == "image_url") {
                        check_cancel(token);
                        auto image = part.as_object().if_contains("image_url");
                        if (!image || !image->is_object())
                            continue;
                        auto url = string_or(image->as_object(), "url");
                        if (url.starts_with("https://")) {
                            auto remaining = cfg.limits.request_bytes - size + url.size();
                            if (remaining <= 128)
                                throw Error(413, "media_limit",
                                            "Remote images exceed the cumulative request budget");
                            auto uri = fetch_public_media(url, (remaining - 128) / 4 * 3, cfg.proxy,
                                                          token);
                            if (uri.size() > remaining)
                                throw Error(413, "media_limit",
                                            "Remote images exceed the cumulative request budget");
                            size = size - url.size() + uri.size();
                            image->as_object()["url"] = std::move(uri);
                        }
                    }
            }
    }
    void generate(const std::shared_ptr<Work> &work) {
        auto cfg = config();
        auto token = work->pipe->cancellation.get_token();
        struct Trace {
            const Work &work;
            Clock::time_point start = Clock::now(), normalized = start;
            std::string error;
            unsigned attempts = 0;
            ~Trace() {
                try {
                    auto elapsed = [](auto begin, auto end) {
                        return std::chrono::duration_cast<std::chrono::milliseconds>(end - begin)
                            .count();
                    };
                    std::cerr << json::serialize(json::object{
                                     {"event", "generation"},
                                     {"request_id", work.id},
                                     {"queue_ms", elapsed(work.queued, start)},
                                     {"normalize_ms", elapsed(start, normalized)},
                                     {"generation_ms", elapsed(normalized, Clock::now())},
                                     {"input_bytes", work.input_bytes},
                                     {"event_bytes", work.pipe->produced_bytes.load()},
                                     {"attempts", attempts},
                                     {"error", error}})
                              << '\n';
                } catch (...) {
                }
            }
        } trace{*work};
        {
            std::lock_guard lock(queue_mutex_);
            active_ = work->pipe;
        }
        struct Clear {
            Service *s;
            ~Clear() {
                std::lock_guard lock(s->queue_mutex_);
                s->active_.reset();
            }
        } clear{this};
        work->pipe->started = true;
        try {
            check_cancel(token);
            gateway_->reconfigure(cfg);
            if (Clock::now() - work->queued > std::chrono::seconds(cfg.limits.queue_seconds))
                throw Error(504, "queue_timeout", "Queue wait exceeded limit");
            if (work->protocol == Protocol::openai)
                prepare_media(work->body, cfg, token);
            auto request = work->protocol == Protocol::gemini
                               ? normalize_gemini(work->model, work->body, work->stream, cfg)
                               : normalize_openai(work->body, cfg, &store_, work->principal);
            request.principal = work->principal;
            work->model = request.model;
            work->include_usage = request.include_usage;
            trace.normalized = Clock::now();
            json::object().swap(work->body);
            std::vector<std::string> excluded;
            std::optional<Error> previous;
            bool delivered = false;
            for (unsigned attempt = 0; attempt < cfg.limits.max_attempts; ++attempt) {
                trace.attempts = attempt + 1;
                check_cancel(token);
                auto account = store_.choose(request.model, excluded);
                if (!account) {
                    if (previous)
                        throw *previous;
                    throw Error(503, "no_account",
                                "No usable account for this model; import or reset credentials "
                                "with the CLI");
                }
                json::object usage;
                try {
                    bool received = false;
                    gateway_->generate(
                        request, *account,
                        [&](json::object event) {
                            check_cancel(token);
                            received = true;
                            if (auto p = event.if_contains("usageMetadata"); p && p->is_object())
                                usage = p->as_object();
                            if (has_content(event))
                                delivered = true;
                            work->pipe->push(
                                std::move(event),
                                Clock::now() + std::chrono::seconds(
                                                   request.stream ? cfg.limits.stream_idle_seconds
                                                                  : cfg.limits.generation_seconds));
                        },
                        token);
                    if (!received)
                        throw Error(502, "empty_upstream",
                                    "Upstream returned no generation events");
                    store_.success(account->id, request.model, usage);
                    work->pipe->finish();
                    return;
                } catch (const Error &e) {
                    if (token.stop_requested())
                        throw;
                    auto kind = e.status == 429   ? "rate_limit"
                                : e.status == 403 ? "permission"
                                : (e.status == 401 || e.code == "login_expired" ||
                                   e.code == "session_expired")
                                    ? "login_expired"
                                    : "transient";
                    if (e.status == 400 || e.status == 413 || e.code == "browser_incompatible" ||
                        e.code == "unsupported_adapter" || e.code == "consumer_timeout")
                        throw;
                    store_.failure(account->id, request.model, kind, e.retry_after);
                    previous = e;
                    excluded.push_back(account->id);
                    if (delivered || attempt + 1 == cfg.limits.max_attempts)
                        throw;
                    gateway_->close();
                }
            }
            throw Error(502, "generation_failed", "Generation failed");
        } catch (const Error &e) {
            trace.error = e.code;
            work->pipe->finish(e);
        } catch (const std::exception &) {
            trace.error = "internal";
            work->pipe->finish(Error(500, "internal", "Generation failed internally"));
        }
    }
};
std::string decode_url(std::string_view source) {
    std::string out;
    for (std::size_t i = 0; i < source.size(); ++i) {
        if (source[i] == '%' && i + 2 < source.size()) {
            unsigned value = 0;
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                if (c >= 'A' && c <= 'F')
                    return c - 'A' + 10;
                return -1;
            };
            auto a = hex(source[i + 1]), b = hex(source[i + 2]);
            if (a < 0 || b < 0)
                throw Error(400, "bad_url", "Invalid URL encoding");
            value = static_cast<unsigned>(a * 16 + b);
            if (!value)
                throw Error(400, "bad_url", "NUL in URL");
            out += static_cast<char>(value);
            i += 2;
        } else
            out += source[i] == '+' ? ' ' : source[i];
    }
    return out;
}
std::string token_from(const http::request<http::string_body> &request) {
    std::string_view target(request.target().data(), request.target().size());
    auto query = target.find('?');
    if (query != std::string_view::npos) {
        auto tail = target.substr(query + 1);
        while (!tail.empty()) {
            auto end = tail.find('&');
            auto pair = tail.substr(0, end);
            auto eq = pair.find('=');
            if (pair.substr(0, eq) == "key" && eq != std::string_view::npos)
                return decode_url(pair.substr(eq + 1));
            if (end == std::string_view::npos)
                break;
            tail.remove_prefix(end + 1);
        }
    }
    for (auto header : {"x-goog-api-key", "x-api-key"}) {
        auto it = request.find(header);
        if (it != request.end())
            return std::string(it->value());
    }
    auto h = request[http::field::authorization];
    if (h.starts_with("Bearer "))
        return std::string(h.substr(7));
    return {};
}
net::awaitable<void> send_json(beast::tcp_stream &stream, unsigned status,
                               const json::object &object) {
    http::response<http::string_body> response(static_cast<http::status>(status), 11);
    response.set(http::field::server, "ais2api");
    response.set(http::field::content_type, "application/json; charset=utf-8");
    response.keep_alive(false);
    response.body() = json::serialize(object);
    response.prepare_payload();
    stream.expires_after(15s);
    co_await http::async_write(stream, response, net::use_awaitable);
}
net::awaitable<void> json_sse(beast::tcp_stream &stream, const json::object &object,
                              std::size_t window) {
    auto write = [&](std::string_view part) -> net::awaitable<void> {
        stream.expires_after(30s);
        co_await net::async_write(stream, http::make_chunk(net::buffer(part.data(), part.size())),
                                  net::use_awaitable);
    };
    co_await write("data: ");
    json::serializer serializer;
    serializer.reset(&object);
    std::vector<char> block(std::max<std::size_t>(1, std::min<std::size_t>(16384, window)));
    while (!serializer.done()) {
        auto part = serializer.read(block.data(), block.size());
        co_await write(part);
    }
    co_await write("\n\n");
}
net::awaitable<std::optional<json::object>> next_event(const std::shared_ptr<Work> &work,
                                                       unsigned queue_seconds) {
    while (true) {
        // Arm before inspecting the pipe. Producer notifications are posted to
        // this executor, so no notification can be lost before async_wait starts.
        work->wakeup->expires_at(work->pipe->started
                                     ? Clock::time_point::max()
                                     : work->queued + std::chrono::seconds(queue_seconds));
        check_cancel(work->pipe->cancellation.get_token());
        if (auto event = work->pipe->take())
            co_return event;
        if (work->pipe->done()) {
            if (auto error = work->pipe->error())
                throw *error;
            co_return std::nullopt;
        }
        if (!work->pipe->started &&
            Clock::now() - work->queued > std::chrono::seconds(queue_seconds)) {
            work->pipe->cancel();
            throw Error(504, "queue_timeout", "Queue wait exceeded limit");
        }
        boost::system::error_code ec;
        co_await work->wakeup->async_wait(net::redirect_error(net::use_awaitable, ec));
        if (ec && ec != net::error::operation_aborted)
            throw boost::system::system_error(ec);
    }
}
struct ConnectionState {
    bool committed = false;
    bool openai = false;
};
// Admission limits total live request bytes before reading bodies. Browser/JSON
// objects have separate bounds, and the scheduler admits one generation at once.
std::atomic<std::size_t> admitted_bytes{0};
struct Admission {
    std::size_t bytes = 0;
    ~Admission() { admitted_bytes.fetch_sub(bytes); }
    bool reserve(std::size_t n) {
        auto current = admitted_bytes.load();
        do {
            if (n > 256ull * 1024 * 1024 - current)
                return false;
        } while (!admitted_bytes.compare_exchange_weak(current, current + n));
        bytes = n;
        return true;
    }
};
net::awaitable<void> handle(beast::tcp_stream &stream, Service &service, ConnectionState &state) {
    auto cfg = service.config();
    beast::flat_buffer input(64 * 1024);
    http::request_parser<http::string_body> parser;
    parser.header_limit(16384);
    parser.body_limit(cfg.limits.request_bytes);
    stream.expires_after(30s);
    co_await http::async_read_header(stream, input, parser, net::use_awaitable);
    auto claimed = parser.content_length();
    auto reservation = claimed ? *claimed : (parser.chunked() ? cfg.limits.request_bytes : 0);
    if (reservation > cfg.limits.request_bytes)
        throw Error(413, "request_limit", "Request exceeds configured size limit");
    Admission admission;
    if (!admission.reserve(static_cast<std::size_t>(reservation) * 4 + 65536))
        throw Error(429, "memory_budget", "Request admission memory budget is exhausted");
    if (parser.get().find(http::field::content_encoding) != parser.get().end())
        throw Error(415, "content_encoding", "Compressed request bodies are not supported");
    co_await http::async_read(stream, input, parser, net::use_awaitable);
    auto request = parser.release();
    std::string target(request.target());
    auto path = target.substr(0, target.find('?'));
    state.openai = path.starts_with("/v1/");
    if (request.method() == http::verb::get && path == "/health") {
        co_await send_json(stream, 200, {{"status", "alive"}});
        co_return;
    }
    if (request.method() == http::verb::get && path == "/ready") {
        auto status = service.control({{"command", "status"}});
        bool ready = bool_or(status, "ready");
        co_await send_json(
            stream, ready ? 200 : 503,
            {{"ready", ready}, {"scope", "local_prerequisites"}, {"google_verified", false}});
        co_return;
    }
    bool models = request.method() == http::verb::get &&
                  (path == "/v1/models" || path.starts_with("/v1/models/") ||
                   path == "/v1beta/models" || path.starts_with("/v1beta/models/"));
    bool openai = request.method() == http::verb::post && path == "/v1/chat/completions";
    bool gemini = request.method() == http::verb::post && path.starts_with("/v1beta/models/") &&
                  (path.ends_with(":generateContent") || path.ends_with(":streamGenerateContent"));
    if (!models && !openai && !gemini)
        throw Error(404, "route_not_found", "Route is not implemented");
    auto principal = service.store().authenticate(token_from(request));
    if (!principal)
        throw Error(401, "authentication", "A valid API key is required");
    if (models) {
        auto operation = service.control({{"command", "models.list"}});
        auto operation_id = string_or(operation, "operation_id");
        auto executor = co_await net::this_coro::executor;
        net::steady_timer polling(executor);
        auto until = Clock::now() + 90s;
        while (true) {
            auto result = service.control({{"command", "operation.status"}, {"id", operation_id}});
            auto status = string_or(result, "status");
            if (status == "done")
                break;
            if (status == "failed") {
                auto e = result.at("error").as_object();
                throw Error(static_cast<int>(e.at("status").as_int64()), string_or(e, "code"),
                            string_or(e, "message"));
            }
            if (Clock::now() > until)
                throw Error(504, "model_discovery_timeout",
                            "Model discovery is still queued or preparing the browser");
            polling.expires_after(25ms);
            co_await polling.async_wait(net::use_awaitable);
        }
        json::array list;
        auto available = service.cached_models();
        for (auto &value : available) {
            if (!value.is_object())
                continue;
            auto object = value.as_object();
            auto id = string_or(object, "id", string_or(object, "name"));
            if (id.starts_with("models/"))
                id.erase(0, 7);
            if (id.empty())
                continue;
            if (state.openai)
                list.push_back(
                    json::object{{"id", id},
                                 {"object", "model"},
                                 {"created", 0},
                                 {"owned_by", "google"},
                                 {"ais2api", {{"source", string_or(object, "source", "cache")}}}});
            else {
                object.erase("id");
                object["name"] = "models/" + id;
                object["supportedGenerationMethods"] =
                    json::array{"generateContent", "streamGenerateContent"};
                list.push_back(std::move(object));
            }
        }
        auto prefix = state.openai ? std::string("/v1/models/") : std::string("/v1beta/models/");
        if (path.starts_with(prefix)) {
            auto id = decode_url(path.substr(prefix.size()));
            for (auto &item : list) {
                auto &object = item.as_object();
                auto name = string_or(object, state.openai ? "id" : "name");
                if (name == id || name == "models/" + id) {
                    co_await send_json(stream, 200, object);
                    co_return;
                }
            }
            throw Error(404, "model_not_found",
                        "Model not found in discovered or configured models");
        }
        co_await send_json(stream, 200,
                           state.openai
                               ? json::object{{"object", "list"}, {"data", std::move(list)}}
                               : json::object{{"models", std::move(list)}});
        co_return;
    }
    auto content_type = request[http::field::content_type];
    if (!content_type.starts_with("application/json"))
        throw Error(415, "content_type", "Content-Type must be application/json");
    auto input_bytes = request.body().size();
    boost::system::error_code ec;
    auto value = json::parse(request.body(), ec);
    if (ec || !value.is_object())
        throw Error(400, "invalid_json", "Request body must be a JSON object");
    std::string().swap(request.body());
    auto work = std::make_shared<Work>();
    work->input_bytes = input_bytes;
    work->protocol = openai ? Protocol::openai : Protocol::gemini;
    work->principal = *principal;
    work->body = std::move(value.as_object());
    work->stream =
        openai ? bool_or(work->body, "stream") : path.ends_with(":streamGenerateContent");
    work->model =
        openai ? string_or(work->body, "model") : decode_url(path.substr(15, path.find(':') - 15));
    work->pipe = std::make_shared<EventPipe>(cfg.limits.object_bytes);
    auto executor = co_await net::this_coro::executor;
    work->wakeup = std::make_shared<net::steady_timer>(executor);
    work->pipe->set_wakeup([executor, weak = std::weak_ptr<net::steady_timer>(work->wakeup)] {
        net::post(executor, [weak] {
            if (auto timer = weak.lock())
                timer->cancel();
        });
    });
    if (!service.enqueue(work))
        throw Error(429, "queue_full", "Generation queue is full");
    struct Cancel {
        std::shared_ptr<EventPipe> pipe;
        ~Cancel() {
            pipe->disable_wakeup();
            pipe->cancel();
        }
    } cancel{work->pipe};
    // Connections are deliberately not reused: readability after the complete
    // request detects FIN/RST (or unsupported pipelining) and cancels generation.
    stream.socket().async_wait(
        tcp::socket::wait_read,
        [weak = std::weak_ptr<EventPipe>(work->pipe)](boost::system::error_code error) {
            // Windows IOCP reports a reset as an error from wait_read. It is
            // still a disconnected consumer; only local cancellation of this
            // monitor should be ignored.
            if (error != net::error::operation_aborted)
                if (auto pipe = weak.lock())
                    pipe->cancel();
        });
    if (!work->stream) {
        json::object aggregate;
        while (auto event = co_await next_event(work, cfg.limits.queue_seconds)) {
            merge_event(aggregate, *event, cfg.limits.object_bytes);
            work->pipe->acknowledge();
        }
        auto output = openai ? openai_response(aggregate, work->model, work->id, &service.store(),
                                               work->principal)
                             : aggregate;
        co_await send_json(stream, 200, output);
        co_return;
    }
    auto event = co_await next_event(work, cfg.limits.queue_seconds);
    if (!event)
        throw Error(502, "empty_upstream", "Upstream returned no content");
    http::response<http::empty_body> response(http::status::ok, 11);
    response.set(http::field::content_type, "text/event-stream");
    response.set(http::field::cache_control, "no-cache");
    response.set("X-Accel-Buffering", "no");
    response.keep_alive(false);
    response.chunked(true);
    http::response_serializer<http::empty_body> header(response);
    stream.expires_after(15s);
    co_await http::async_write_header(stream, header, net::use_awaitable);
    state.committed = true;
    const auto created = unix_seconds();
    std::optional<json::value> final_usage;
    std::set<std::int64_t> roles_sent;
    while (event) {
        auto output =
            openai ? openai_delta(*event, work->model, work->id, &service.store(), work->principal)
                   : *event;
        if (openai) {
            output["created"] = created;
            if (auto usage = output.if_contains("usage"))
                final_usage = *usage;
            output.erase("usage");
            if (work->include_usage)
                output["usage"] = nullptr;
            for (auto &choice : output.at("choices").as_array()) {
                auto &item = choice.as_object();
                auto index = json::value_to<std::int64_t>(item.at("index"));
                if (!roles_sent.insert(index).second)
                    item.at("delta").as_object().erase("role");
            }
        }
        co_await json_sse(stream, output, cfg.limits.relay_bytes);
        work->pipe->acknowledge();
        event = co_await next_event(work, cfg.limits.queue_seconds);
    }
    if (openai && work->include_usage && final_usage) {
        json::object usage_chunk{{"id", work->id},           {"object", "chat.completion.chunk"},
                                 {"created", created},       {"model", work->model},
                                 {"choices", json::array{}}, {"usage", std::move(*final_usage)}};
        co_await json_sse(stream, usage_chunk, cfg.limits.relay_bytes);
    }
    if (openai)
        co_await net::async_write(stream, http::make_chunk(net::buffer("data: [DONE]\n\n", 14)),
                                  net::use_awaitable);
    co_await net::async_write(stream, http::make_chunk_last(), net::use_awaitable);
}
net::awaitable<void> connection(tcp::socket socket, Service &service,
                                std::shared_ptr<ConnectionSlot> slot) {
    (void)slot;
    beast::tcp_stream stream(std::move(socket));
    ConnectionState state;
    std::optional<Error> error;
    try {
        co_await handle(stream, service, state);
    } catch (const Error &e) {
        error = e;
    } catch (const boost::system::system_error &e) {
        if (e.code() == http::error::body_limit)
            error = Error(413, "request_limit", "Request exceeds configured size limit");
        else if (e.code() == beast::error::timeout)
            error = Error(504, "network_timeout", "HTTP I/O timed out");
    } catch (const std::exception &) {
        error = Error(500, "internal", "Request failed internally");
    }
    if (error) {
        try {
            if (state.committed) {
                co_await json_sse(stream, error_json(*error, state.openai), 16384);
                co_await net::async_write(stream, http::make_chunk_last(), net::use_awaitable);
            } else if (error->status != 499)
                co_await send_json(stream, static_cast<unsigned>(error->status),
                                   error_json(*error, state.openai));
        } catch (...) {
        }
    }
    boost::system::error_code ignored;
    stream.socket().cancel(ignored);
    stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
    stream.socket().close(ignored);
}
net::awaitable<void> accept_loop(tcp::acceptor &acceptor, Service &service) {
    while (!service.stopping()) {
        boost::system::error_code ec;
        auto socket = co_await acceptor.async_accept(net::redirect_error(net::use_awaitable, ec));
        if (ec)
            co_return;
        if (live_connections.fetch_add(1) >= 32) {
            live_connections.fetch_sub(1);
            socket.close(ec);
            continue;
        }
        auto slot = std::make_shared<ConnectionSlot>();
        net::co_spawn(acceptor.get_executor(),
                      connection(std::move(socket), service, std::move(slot)), net::detached);
    }
}
} // namespace
int serve(Config config) { return serve_with_backend(std::move(config), {}); }
int serve_with_backend(Config config, std::unique_ptr<GenerationBackend> backend) {
    config.validate();
    InstanceLock owner(config.data_dir);
    Service service(config, std::move(backend));
    net::io_context io(1);
    tcp::endpoint endpoint(net::ip::make_address(config.bind), config.port);
    tcp::acceptor acceptor(io);
    auto check_listener = [&](std::string_view stage, const boost::system::error_code &ec) {
        if (!ec)
            return;
        const std::string reason = ec == net::error::access_denied ? "permission_denied"
                                   : ec == net::error::address_in_use ? "address_in_use"
                                                                     : "listener_error";
        throw Error(503, "http_" + reason,
                    "HTTP listener " + std::string(stage) + " failed for bind=" + config.bind +
                        " port=" + std::to_string(config.port) + " reason=" + reason +
                        " os_error=" + std::to_string(ec.value()) +
                        "; configure a different available port and restart");
    };
    boost::system::error_code listener_error;
    acceptor.open(endpoint.protocol(), listener_error);
    check_listener("open", listener_error);
    acceptor.set_option(net::socket_base::reuse_address(true), listener_error);
    check_listener("set_option", listener_error);
    acceptor.bind(endpoint, listener_error);
    check_listener("bind", listener_error);
    acceptor.listen(32, listener_error);
    check_listener("listen", listener_error);
    ControlServer control(config,
                          [&](const json::object &command) { return service.control(command); });
    net::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](auto, int) {
        service.stop();
        boost::system::error_code ec;
        acceptor.close(ec);
        io.stop();
    });
    net::steady_timer watcher(io);
    std::function<void()> watch;
    watch = [&] {
        watcher.expires_after(100ms);
        watcher.async_wait([&](boost::system::error_code ec) {
            if (ec)
                return;
            if (service.stopping()) {
                boost::system::error_code ignored;
                acceptor.close(ignored);
                io.stop();
            } else
                watch();
        });
    };
    watch();
    std::cerr << json::serialize(json::object{
                     {"event", "listening"}, {"bind", config.bind}, {"port", config.port}})
              << '\n';
    net::co_spawn(io, accept_loop(acceptor, service), net::detached);
    io.run();
    service.shutdown();
    return 0;
}
} // namespace ais2api
