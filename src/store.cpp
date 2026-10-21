// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/store.hpp"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <future>
#include <limits>
#include <sqlite3.h>
#include <sstream>
#include <thread>
namespace ais2api {
namespace {
std::string sqlite_path(const fs::path &path) {
    auto u = path.u8string();
    return std::string(reinterpret_cast<const char *>(u.data()), u.size());
}
struct Statement {
    sqlite3_stmt *p = nullptr;
    Statement(sqlite3 *db, const char *sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &p, nullptr) != SQLITE_OK)
            throw Error(500, "database", sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(p); }
    void text(int i, std::string_view v) {
        if (sqlite3_bind_text(p, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT) !=
            SQLITE_OK)
            throw Error(500, "database", "Cannot bind database value");
    }
    void integer(int i, std::int64_t v) { sqlite3_bind_int64(p, i, v); }
    bool row() {
        auto r = sqlite3_step(p);
        if (r == SQLITE_ROW)
            return true;
        if (r == SQLITE_DONE)
            return false;
        throw Error(500, "database", sqlite3_errmsg(sqlite3_db_handle(p)));
    }
    std::string str(int i) {
        auto v = sqlite3_column_text(p, i);
        return v ? reinterpret_cast<const char *>(v) : "";
    }
};
std::int64_t next_touch(sqlite3 *db) {
    Statement s(db, "SELECT COALESCE(MAX(touched),0)+1 FROM tools");
    s.row();
    return sqlite3_column_int64(s.p, 0);
}
void exec(sqlite3 *db, const char *sql) {
    char *error = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        std::string s = error ? error : "Database error";
        sqlite3_free(error);
        throw Error(500, "database", s);
    }
}
Account decode(Statement &s) {
    Account a;
    a.id = s.str(0);
    a.name = s.str(1);
    a.email = s.str(2);
    a.auth_user = s.str(3);
    a.storage_state = json::parse(s.str(4)).as_object();
    a.disabled = sqlite3_column_int(s.p, 5) != 0;
    return a;
}
json::object counters(const json::object &source) {
    json::object result;
    for (auto &p : source) {
        if (p.value().is_int64() && p.value().as_int64() >= 0)
            result[p.key()] = p.value();
        else if (p.value().is_uint64())
            result[p.key()] = p.value();
        else if (p.value().is_object())
            result[p.key()] = counters(p.value().as_object());
    }
    return result;
}
void accumulate(json::object &target, const json::object &values) {
    for (auto &p : values) {
        if (p.value().is_object()) {
            auto old = target.if_contains(p.key());
            if (!old || !old->is_object())
                target[p.key()] = json::object{};
            accumulate(target.at(p.key()).as_object(), p.value().as_object());
        } else {
            auto old = target.if_contains(p.key());
            auto next = p.value().is_uint64() ? p.value().as_uint64()
                                              : static_cast<std::uint64_t>(p.value().as_int64());
            std::uint64_t prior = 0;
            if (old && old->is_uint64())
                prior = old->as_uint64();
            else if (old && old->is_int64() && old->as_int64() > 0)
                prior = static_cast<std::uint64_t>(old->as_int64());
            target[p.key()] = next > std::numeric_limits<std::uint64_t>::max() - prior
                                  ? std::numeric_limits<std::uint64_t>::max()
                                  : prior + next;
        }
    }
}
json::object summary(const Account &a) {
    return {{"id", a.id},
            {"name", a.name},
            {"email", a.email},
            {"auth_user", a.auth_user},
            {"disabled", a.disabled}};
}
std::string import_text(std::string text) {
    constexpr std::string_view whitespace = " \t\r\n\f\v";
    auto trim_front = [&] {
        auto first = text.find_first_not_of(whitespace);
        if (first == std::string::npos)
            text.clear();
        else
            text.erase(0, first);
    };
    trim_front();
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.erase(0, 3);
        trim_front();
    }
    // Netscape's last tab can delimit an intentionally empty cookie value.
    // Preserve field whitespace there; remove only surrounding line endings.
    bool netscape = false;
    std::istringstream rows(text);
    std::string row;
    while (std::getline(rows, row))
        if (std::count(row.begin(), row.end(), '\t') >= 6) {
            netscape = true;
            break;
        }
    auto last = text.find_last_not_of(netscape ? std::string_view("\r\n") : whitespace);
    if (last == std::string::npos)
        text.clear();
    else
        text.erase(last + 1);
    return text;
}
json::object state(json::value v) {
    if (v.is_string()) {
        auto raw = import_text(std::string(v.as_string()));
        try {
            v = json::parse(raw);
        } catch (...) {
            json::array cs;
            if (raw.find('\t') != std::string::npos) {
                std::istringstream lines(raw);
                std::string line;
                while (std::getline(lines, line)) {
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    bool http = false;
                    if (line.starts_with("#HttpOnly_")) {
                        http = true;
                        line.erase(0, 10);
                    } else if (line.find_first_not_of(" \t\r") == std::string::npos ||
                               line[0] == '#')
                        continue;
                    std::vector<std::string> columns;
                    std::size_t start = 0;
                    for (int n = 0; n < 6; ++n) {
                        auto end = line.find('\t', start);
                        if (end == std::string::npos)
                            break;
                        columns.push_back(line.substr(start, end - start));
                        start = end + 1;
                    }
                    if (columns.size() != 6)
                        throw Error(400, "invalid_credentials", "Invalid Netscape cookie row");
                    columns.push_back(line.substr(start));
                    json::object cookie{{"domain", columns[0]},
                                        {"path", columns[2]},
                                        {"secure", columns[3] == "TRUE"},
                                        {"httpOnly", http},
                                        {"name", columns[5]},
                                        {"value", columns[6]}};
                    if (columns[4] != "0") {
                        try {
                            std::size_t end = 0;
                            auto expires = std::stoll(columns[4], &end);
                            if (end != columns[4].size())
                                throw std::invalid_argument("expiry");
                            cookie["expires"] = expires;
                        } catch (...) {
                            throw Error(400, "invalid_credentials", "Invalid cookie expiry");
                        }
                    }
                    cs.push_back(cookie);
                }
                return state(json::object{{"cookies", cs}});
            }
            if (raw.starts_with("Cookie:") || raw.starts_with("cookie:"))
                raw.erase(0, 7);
            if (raw.find_first_of("\r\n") != std::string::npos)
                throw Error(400, "invalid_credentials",
                            "Cookie header contains an internal line break");
            std::size_t pos = 0;
            while (pos < raw.size()) {
                auto end = raw.find(';', pos);
                auto part = raw.substr(pos, end == std::string::npos ? end : end - pos);
                auto eq = part.find('=');
                if (eq != std::string::npos) {
                    auto k = part.substr(0, eq);
                    k.erase(0, k.find_first_not_of(" \t"));
                    k.erase(k.find_last_not_of(" \t") + 1);
                    if (!k.empty())
                        cs.push_back(json::object{{"name", k},
                                                  {"value", part.substr(eq + 1)},
                                                  {"domain", ".google.com"},
                                                  {"path", "/"},
                                                  {"secure", true},
                                                  {"httpOnly", true}});
                }
                if (end == std::string::npos)
                    break;
                pos = end + 1;
            }
            v = json::object{{"cookies", cs}};
        }
    }
    if (v.is_array())
        v = json::object{{"cookies", v}};
    if (!v.is_object())
        throw Error(400, "invalid_credentials", "Expected cookie array or storage state");
    auto o = v.as_object();
    auto cookies = o.if_contains("cookies");
    if (!cookies)
        throw Error(400, "invalid_credentials", "Missing cookies");
    if (cookies->is_string())
        return state(*cookies);
    if (cookies->is_object()) {
        json::array mapped;
        for (auto &p : cookies->as_object()) {
            if (!p.value().is_string())
                throw Error(400, "invalid_credentials", "Cookie map values must be strings");
            mapped.push_back(json::object{{"name", p.key()},
                                          {"value", p.value()},
                                          {"domain", ".google.com"},
                                          {"path", "/"},
                                          {"secure", true}});
        }
        o["cookies"] = mapped;
        cookies = o.if_contains("cookies");
    }
    if (!cookies->is_array() || cookies->as_array().empty())
        throw Error(400, "invalid_credentials", "Cookie array is empty");
    for (auto &cv : cookies->as_array()) {
        if (!cv.is_object())
            throw Error(400, "invalid_credentials", "Invalid cookie");
        auto &c = cv.as_object();
        auto domain = string_or(c, "domain");
        if (domain.empty() || domain.find_first_of("/\\ \t\r\n") != std::string::npos ||
            string_or(c, "name").empty() || !c.if_contains("value") || !c.at("value").is_string())
            throw Error(400, "invalid_credentials",
                        "Cookie name, value and valid domain are required");
        bool domain_ok = true;
        for (unsigned char ch : domain)
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '.' || ch == '-'))
                domain_ok = false;
        if (!domain_ok || domain == "." || domain.find("..") != std::string::npos ||
            domain.back() == '.')
            throw Error(400, "invalid_credentials", "Invalid cookie domain");
        auto cookie_name = string_or(c, "name");
        if (cookie_name.find_first_of("\r\n\t ;=") != std::string::npos ||
            string_or(c, "value").find_first_of("\r\n") != std::string::npos)
            throw Error(400, "invalid_credentials", "Invalid cookie name or value");
        for (auto flag : {"secure", "httpOnly"})
            if (auto p = c.if_contains(flag); p && !p->is_bool())
                throw Error(400, "invalid_credentials", "Cookie flags must be boolean");
        if (auto p = c.if_contains("sameSite");
            p && (!p->is_string() || (p->as_string() != "Strict" && p->as_string() != "Lax" &&
                                      p->as_string() != "None")))
            throw Error(400, "invalid_credentials", "Invalid sameSite metadata");
        if (auto p = c.if_contains("expires"); p && !p->is_number())
            throw Error(400, "invalid_credentials", "Cookie expiry must be numeric");
        if (!c.if_contains("path"))
            c["path"] = "/";
        if (!c.at("path").is_string() || !string_or(c, "path").starts_with('/'))
            throw Error(400, "invalid_credentials", "Invalid cookie path");
    }
    if (auto p = o.if_contains("origins"); p && !p->is_array())
        throw Error(400, "invalid_credentials", "Invalid storage origins");
    return o;
}
} // namespace
struct Store::Impl {
    sqlite3 *db = nullptr;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> queue;
    bool stopping = false;
    std::thread worker;
    explicit Impl(const fs::path &directory) {
        secure_directory(directory);
        auto path = directory / "state.sqlite3";
        if (fs::is_symlink(fs::symlink_status(path)))
            throw Error(400, "unsafe_path", "Database cannot be a symbolic link");
        std::promise<void> ready;
        auto f = ready.get_future();
        worker = std::thread([this, path, &ready] {
            try {
                if (sqlite3_open_v2(sqlite_path(path).c_str(), &db,
                                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                        SQLITE_OPEN_NOMUTEX,
                                    nullptr) != SQLITE_OK)
                    throw Error(500, "database", "Cannot open state database");
                sqlite3_busy_timeout(db, 5000);
                {
                    Statement version(db, "PRAGMA user_version");
                    if (version.row() && sqlite3_column_int(version.p, 0) > 1)
                        throw Error(500, "database_version",
                                    "Database was created by a newer ais2api version");
                }
                exec(db, "PRAGMA foreign_keys=ON;PRAGMA journal_mode=WAL;CREATE TABLE IF "
                         "NOT EXISTS accounts(id TEXT PRIMARY KEY,name TEXT,email "
                         "TEXT,auth_user TEXT,state TEXT,disabled INTEGER DEFAULT 0,expired "
                         "INTEGER DEFAULT 0);CREATE TABLE IF NOT EXISTS settings(k TEXT "
                         "PRIMARY KEY,v TEXT);CREATE TABLE IF NOT EXISTS failures(account "
                         "TEXT REFERENCES accounts(id) ON DELETE CASCADE,model TEXT,until "
                         "INTEGER,count INTEGER,PRIMARY KEY(account,model));CREATE TABLE IF "
                         "NOT EXISTS sticky(model TEXT PRIMARY KEY,account TEXT REFERENCES "
                         "accounts(id) ON DELETE CASCADE);CREATE TABLE IF NOT EXISTS "
                         "stats(account TEXT,model TEXT,ok INTEGER DEFAULT 0,failed INTEGER "
                         "DEFAULT 0,usage TEXT DEFAULT '{}',PRIMARY "
                         "KEY(account,model));CREATE TABLE IF NOT EXISTS keys(id TEXT "
                         "PRIMARY KEY,name TEXT,hash TEXT UNIQUE,created INTEGER);CREATE "
                         "TABLE IF NOT EXISTS tools(principal TEXT,call_id TEXT,metadata "
                         "TEXT,created INTEGER,touched INTEGER,PRIMARY "
                         "KEY(principal,call_id));CREATE INDEX IF NOT EXISTS tools_lru ON "
                         "tools(touched);CREATE INDEX IF NOT EXISTS tools_expiry ON "
                         "tools(created);PRAGMA user_version=1;");
                ready.set_value();
            } catch (...) {
                ready.set_exception(std::current_exception());
                if (db)
                    sqlite3_close(db);
                db = nullptr;
                return;
            }
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock lock(mutex);
                    cv.wait(lock, [&] { return stopping || !queue.empty(); });
                    if (stopping && queue.empty())
                        break;
                    task = std::move(queue.front());
                    queue.pop_front();
                }
                task();
            }
            sqlite3_close(db);
            db = nullptr;
        });
        try {
            f.get();
        } catch (...) {
            worker.join();
            throw;
        }
    }
    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        cv.notify_one();
        if (worker.joinable())
            worker.join();
    }
    template <class F> auto run(F f) -> decltype(f()) {
        using R = decltype(f());
        auto task = std::make_shared<std::packaged_task<R()>>(std::move(f));
        auto future = task->get_future();
        {
            std::lock_guard lock(mutex);
            if (stopping)
                throw Error(503, "database", "Store is shutting down");
            queue.emplace_back([task] { (*task)(); });
        }
        cv.notify_one();
        return future.get();
    }
    void put(const Account &a) {
        Statement s(db, "INSERT INTO accounts(id,name,email,auth_user,state,disabled) "
                        "VALUES(?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET "
                        "name=excluded.name,email=excluded.email,auth_user=excluded."
                        "auth_user,state=excluded.state,disabled=excluded.disabled");
        s.text(1, a.id);
        s.text(2, a.name);
        s.text(3, a.email);
        s.text(4, a.auth_user);
        s.text(5, json::serialize(a.storage_state));
        s.integer(6, a.disabled);
        s.row();
    }
};
Store::Store(const fs::path &d) : impl_(std::make_unique<Impl>(d)) {}
Store::~Store() = default;
std::vector<Account> Store::accounts() {
    return impl_->run([&] {
        std::vector<Account> out;
        Statement s(impl_->db, "SELECT id,name,email,auth_user,state,disabled FROM "
                               "accounts ORDER BY rowid");
        while (s.row())
            out.push_back(decode(s));
        return out;
    });
}
Account Store::account(std::string_view id) {
    return impl_->run([&] {
        Statement s(impl_->db, "SELECT id,name,email,auth_user,state,disabled FROM "
                               "accounts WHERE id=?");
        s.text(1, id);
        if (!s.row())
            throw Error(404, "account_not_found", "Account not found");
        return decode(s);
    });
}
json::object Store::normalize_storage_state(const json::value &input) { return state(input); }
json::array Store::merge_verified_accounts(const std::vector<Account> &input) {
    auto parsed = input;
    for (auto &a : parsed) {
        if (a.email.empty() || a.email.size() > 320 || a.id.empty() ||
            a.email.find_first_of(" \t\r\n") != std::string::npos ||
            a.auth_user.empty() || a.auth_user.size() > 10 ||
            a.auth_user.find_first_not_of("0123456789") != std::string::npos)
            throw Error(400, "invalid_account", "Verified account identity required");
        a.storage_state = state(a.storage_state);
        a.disabled = false;
    }
    return impl_->run([&] {
        json::array out;
        exec(impl_->db, "BEGIN IMMEDIATE");
        try {
            for (auto a : parsed) {
                Statement existing(impl_->db, "SELECT id,name FROM accounts WHERE email=? COLLATE NOCASE ORDER BY rowid LIMIT 1");
                existing.text(1, a.email);
                if (existing.row()) {
                    a.id = existing.str(0);
                    a.name = existing.str(1);
                } else {
                    // Candidate IDs must never overwrite an unrelated stored identity.
                    Statement collision(impl_->db, "SELECT 1 FROM accounts WHERE id=?");
                    collision.text(1, a.id);
                    if (collision.row())
                        a.id = "acc_" + random_hex(16);
                }
                impl_->put(a);
                Statement reset(impl_->db, "UPDATE accounts SET expired=0 WHERE id=?");
                reset.text(1, a.id); reset.row();
                // '*' is the rate-limit cooldown and must survive credential refresh.
                Statement failures(impl_->db, "DELETE FROM failures WHERE account=? AND model<>'*'");
                failures.text(1, a.id); failures.row();
                out.push_back(summary(a));
            }
            exec(impl_->db, "COMMIT");
        } catch (...) {
            exec(impl_->db, "ROLLBACK");
            throw;
        }
        return out;
    });
}
void Store::put_account(const Account &a) {
    auto copy = a;
    if (copy.id.empty())
        throw Error(400, "invalid_account", "Account ID required");
    copy.storage_state = state(copy.storage_state);
    impl_->run([&] { impl_->put(copy); });
}
json::array Store::import_accounts(const json::value &input, std::string name) {
    json::value v = input;
    if (v.is_string()) {
        v = json::string(import_text(std::string(v.as_string())));
        try {
            v = json::parse(v.as_string());
        } catch (...) {
        }
    }
    json::array entries;
    if (v.is_object() &&
        (v.as_object().if_contains("accounts") || v.as_object().if_contains("profiles"))) {
        auto &o = v.as_object();
        auto p = o.if_contains("accounts");
        if (!p)
            p = o.if_contains("profiles");
        if (!p->is_array())
            throw Error(400, "invalid_credentials", "Bundle must contain account array");
        entries = p->as_array();
    } else if (v.is_array() && !v.as_array().empty() && v.as_array()[0].is_object() &&
               (v.as_array()[0].as_object().if_contains("cookies") ||
                v.as_array()[0].as_object().if_contains("storage_state")))
        entries = v.as_array();
    else
        entries.push_back(v);
    std::vector<Account> parsed;
    for (auto &entry : entries) {
        Account a;
        a.id = "acc_" + random_hex(8);
        a.name = name.empty() ? "Imported account" : name;
        if (entry.is_object()) {
            auto &o = entry.as_object();
            a.name = string_or(o, "name", a.name);
            a.email = string_or(o, "email");
            a.auth_user = string_or(o, "auth_user", "0");
            if (auto p = o.if_contains("storage_state"))
                a.storage_state = state(*p);
            else
                a.storage_state = state(entry);
        } else
            a.storage_state = state(entry);
        if (a.auth_user.empty() || a.auth_user.size() > 10 ||
            a.auth_user.find_first_not_of("0123456789") != std::string::npos)
            throw Error(400, "invalid_credentials", "auth_user must be a decimal account index");
        parsed.push_back(std::move(a));
    }
    if (parsed.empty())
        throw Error(400, "invalid_credentials", "No accounts supplied");
    return impl_->run([&] {
        json::array out;
        exec(impl_->db, "BEGIN IMMEDIATE");
        try {
            for (auto &a : parsed) {
                impl_->put(a);
                out.push_back(summary(a));
            }
            exec(impl_->db, "COMMIT");
        } catch (...) {
            exec(impl_->db, "ROLLBACK");
            throw;
        }
        return out;
    });
}
void Store::remove_account(std::string_view id) {
    impl_->run([&] {
        Statement s(impl_->db, "DELETE FROM accounts WHERE id=?");
        s.text(1, id);
        s.row();
    });
}
void Store::select_account(std::string_view id) {
    impl_->run([&] {
        Statement exists(impl_->db, "SELECT 1 FROM accounts WHERE id=?");
        exists.text(1, id);
        if (!exists.row())
            throw Error(404, "account_not_found", "Account not found");
        Statement s(impl_->db, "INSERT INTO settings(k,v) VALUES('selected',?) ON "
                               "CONFLICT(k) DO UPDATE SET v=excluded.v");
        s.text(1, id);
        s.row();
        exec(impl_->db, "DELETE FROM sticky");
    });
}
void Store::reset_account(std::string_view id) {
    impl_->run([&] {
        exec(impl_->db, "BEGIN IMMEDIATE");
        try {
            Statement a(impl_->db, "UPDATE accounts SET expired=0,disabled=0 WHERE id=?");
            a.text(1, id);
            a.row();
            Statement f(impl_->db, "DELETE FROM failures WHERE account=?");
            f.text(1, id);
            f.row();
            exec(impl_->db, "COMMIT");
        } catch (...) {
            exec(impl_->db, "ROLLBACK");
            throw;
        }
    });
}
std::optional<Account> Store::choose(std::string_view model,
                                     const std::vector<std::string> &excluded) {
    return impl_->run([&]() -> std::optional<Account> {
        Statement s(impl_->db,
                    "SELECT a.id,a.name,a.email,a.auth_user,a.state,a.disabled FROM "
                    "accounts a WHERE a.disabled=0 AND a.expired=0 AND NOT EXISTS(SELECT 1 "
                    "FROM failures f WHERE f.account=a.id AND (f.model=? OR f.model='*') "
                    "AND f.until>?) ORDER BY (a.id=(SELECT account FROM sticky WHERE "
                    "model=?)) DESC,(a.id=(SELECT v FROM settings WHERE k='selected')) "
                    "DESC,a.rowid");
        s.text(1, model);
        s.integer(2, unix_seconds());
        s.text(3, model);
        while (s.row()) {
            auto a = decode(s);
            if (std::find(excluded.begin(), excluded.end(), a.id) != excluded.end())
                continue;
            return a;
        }
        return {};
    });
}
void Store::failure(std::string_view id, std::string_view model, std::string_view kind,
                    unsigned retry) {
    impl_->run([&] {
        exec(impl_->db, "BEGIN IMMEDIATE");
        try {
            if (kind == "login" || kind == "expired" || kind == "login_expired" ||
                kind == "authentication") {
                Statement s(impl_->db, "UPDATE accounts SET expired=1 WHERE id=?");
                s.text(1, id);
                s.row();
            } else {
                std::string m = (kind == "429" || kind == "rate_limit") ? "*" : std::string(model);
                Statement prior(impl_->db,
                                "SELECT count FROM failures WHERE account=? AND model=?");
                prior.text(1, id);
                prior.text(2, m);
                int count = prior.row() ? sqlite3_column_int(prior.p, 0) : 0;
                auto delay = std::min<unsigned>(
                    600, std::max<unsigned>(retry, 60u << std::clamp(count, 0, 4)));
                auto until =
                    (kind == "permission" || kind == "403") ? INT64_MAX : unix_seconds() + delay;
                Statement s(impl_->db,
                            "INSERT INTO failures VALUES(?,?,?,?) ON CONFLICT(account,model) "
                            "DO UPDATE SET until=excluded.until,count=excluded.count");
                s.text(1, id);
                s.text(2, m);
                s.integer(3, until);
                s.integer(4, count + 1);
                s.row();
            }
            Statement st(impl_->db, "INSERT INTO stats(account,model,failed) VALUES(?,?,1) ON "
                                    "CONFLICT(account,model) DO UPDATE SET failed=failed+1");
            st.text(1, id);
            st.text(2, model);
            st.row();
            exec(impl_->db, "COMMIT");
        } catch (...) {
            exec(impl_->db, "ROLLBACK");
            throw;
        }
    });
}
void Store::success(std::string_view id, std::string_view model, const json::object &usage) {
    impl_->run([&] {
        exec(impl_->db, "BEGIN IMMEDIATE");
        try {
            json::object totals;
            Statement previous(impl_->db, "SELECT usage FROM stats WHERE account=? AND model=?");
            previous.text(1, id);
            previous.text(2, model);
            if (previous.row())
                totals = json::parse(previous.str(0)).as_object();
            accumulate(totals, counters(usage));
            Statement s(impl_->db,
                        "INSERT INTO stats(account,model,ok,usage) VALUES(?,?,1,?) ON "
                        "CONFLICT(account,model) DO UPDATE SET ok=ok+1,usage=excluded.usage");
            s.text(1, id);
            s.text(2, model);
            s.text(3, json::serialize(totals));
            s.row();
            Statement sticky(impl_->db, "INSERT INTO sticky VALUES(?,?) ON CONFLICT(model) DO "
                                        "UPDATE SET account=excluded.account");
            sticky.text(1, model);
            sticky.text(2, id);
            sticky.row();
            Statement reset(impl_->db, "DELETE FROM failures WHERE account=? AND "
                                       "(model=? OR model='*') AND until<=?");
            reset.text(1, id);
            reset.text(2, model);
            reset.integer(3, unix_seconds());
            reset.row();
            exec(impl_->db, "COMMIT");
        } catch (...) {
            exec(impl_->db, "ROLLBACK");
            throw;
        }
    });
}
json::object Store::stats() {
    return impl_->run([&] {
        json::array rows;
        Statement s(impl_->db, "SELECT account,model,ok,failed,usage FROM stats "
                               "ORDER BY account,model");
        while (s.row())
            rows.push_back(json::object{{"account", s.str(0)},
                                        {"model", s.str(1)},
                                        {"success", sqlite3_column_int64(s.p, 2)},
                                        {"failure", sqlite3_column_int64(s.p, 3)},
                                        {"usage", json::parse(s.str(4))}});
        return json::object{{"records", rows}};
    });
}
json::array Store::account_status() {
    return impl_->run([&] {
        json::array rows;
        Statement s(impl_->db, "SELECT id,name,email,auth_user,state,disabled,expired FROM "
                               "accounts ORDER BY rowid");
        while (s.row()) {
            auto o = summary(decode(s));
            o["expired"] = sqlite3_column_int(s.p, 6) != 0;
            json::array failures;
            Statement f(impl_->db, "SELECT model,until,count FROM failures WHERE account=?");
            f.text(1, s.str(0));
            while (f.row())
                failures.push_back(json::object{{"model", f.str(0)},
                                                {"until", sqlite3_column_int64(f.p, 1)},
                                                {"count", sqlite3_column_int(f.p, 2)}});
            o["failures"] = failures;
            rows.push_back(o);
        }
        return rows;
    });
}
json::object Store::create_key(std::string name) {
    auto secret = "sk-ais2api-" + random_hex(32);
    auto id = "key_" + random_hex(8);
    impl_->run([&] {
        Statement s(impl_->db, "INSERT INTO keys VALUES(?,?,?,?)");
        s.text(1, id);
        s.text(2, name);
        s.text(3, sha256(secret));
        s.integer(4, unix_seconds());
        s.row();
    });
    return {{"id", id}, {"name", name}, {"secret", secret}};
}
json::array Store::keys() {
    return impl_->run([&] {
        json::array rows;
        Statement s(impl_->db, "SELECT id,name,created FROM keys ORDER BY created,id");
        while (s.row())
            rows.push_back(json::object{
                {"id", s.str(0)}, {"name", s.str(1)}, {"created", sqlite3_column_int64(s.p, 2)}});
        return rows;
    });
}
void Store::revoke_key(std::string_view id) {
    impl_->run([&] {
        Statement s(impl_->db, "DELETE FROM keys WHERE id=?");
        s.text(1, id);
        s.row();
    });
}
std::optional<std::string> Store::authenticate(std::string_view secret) {
    return impl_->run([&]() -> std::optional<std::string> {
        Statement s(impl_->db, "SELECT id FROM keys WHERE hash=?");
        s.text(1, sha256(secret));
        if (s.row())
            return s.str(0);
        return {};
    });
}
void Store::put_tool(std::string_view principal, std::string_view call,
                     const json::object &metadata) {
    impl_->run([&] {
        if (principal.empty() || call.empty())
            throw Error(400, "invalid_tool", "Principal and call ID required");
        exec(impl_->db, "BEGIN IMMEDIATE");
        try {
            Statement clean(impl_->db, "DELETE FROM tools WHERE created<=?");
            clean.integer(1, unix_seconds() - 86400);
            clean.row();
            Statement s(impl_->db, "INSERT INTO tools VALUES(?,?,?,?,?) ON "
                                   "CONFLICT(principal,call_id) DO UPDATE SET "
                                   "metadata=excluded.metadata,created=excluded."
                                   "created,touched=excluded.touched");
            s.text(1, principal);
            s.text(2, call);
            s.text(3, json::serialize(metadata));
            s.integer(4, unix_seconds());
            s.integer(5, next_touch(impl_->db));
            s.row();
            exec(impl_->db, "DELETE FROM tools WHERE rowid IN(SELECT rowid FROM tools ORDER BY "
                            "touched DESC,rowid DESC LIMIT -1 OFFSET 4096);COMMIT;");
        } catch (...) {
            exec(impl_->db, "ROLLBACK");
            throw;
        }
    });
}
std::optional<json::object> Store::get_tool(std::string_view principal, std::string_view call) {
    return impl_->run([&]() -> std::optional<json::object> {
        Statement s(impl_->db, "SELECT metadata FROM tools WHERE principal=? AND "
                               "call_id=? AND created>?");
        s.text(1, principal);
        s.text(2, call);
        s.integer(3, unix_seconds() - 86400);
        if (!s.row())
            return {};
        auto result = json::parse(s.str(0)).as_object();
        Statement touch(impl_->db, "UPDATE tools SET touched=? WHERE principal=? AND call_id=?");
        touch.integer(1, next_touch(impl_->db));
        touch.text(2, principal);
        touch.text(3, call);
        touch.row();
        return result;
    });
}
void Store::backup(const fs::path &target) {
    impl_->run([&] {
        if (fs::exists(target) || fs::is_symlink(fs::symlink_status(target)))
            throw Error(409, "backup_exists", "Backup target already exists");
        if (!target.parent_path().empty())
            secure_directory(target.parent_path());
        sqlite3 *dest = nullptr;
        if (sqlite3_open(sqlite_path(target).c_str(), &dest) != SQLITE_OK) {
            if (dest)
                sqlite3_close(dest);
            throw Error(500, "backup", "Cannot open backup target");
        }
        auto b = sqlite3_backup_init(dest, "main", impl_->db, "main");
        if (!b) {
            sqlite3_close(dest);
            fs::remove(target);
            throw Error(500, "backup", "Cannot initialize database backup");
        }
        auto rc = sqlite3_backup_step(b, -1);
        auto end = sqlite3_backup_finish(b);
        sqlite3_close(dest);
        if (rc != SQLITE_DONE || end != SQLITE_OK) {
            fs::remove(target);
            throw Error(500, "backup", "Database backup failed");
        }
    });
}
} // namespace ais2api
