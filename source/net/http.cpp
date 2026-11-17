// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "net/http.h"

#include <pthread.h>
#include <stdio.h>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>

#include "core/fs.h"
#include "core/json.h"
#include "core/log.h"
#include "core/log.h"
#include "core/text.h"
#include "net/url.h"
#include "net/archive.h"

namespace lib {

namespace {

pthread_mutex_t g_auth_lock = PTHREAD_MUTEX_INITIALIZER;
std::map<std::string, std::pair<std::string, std::string>> g_credentials;  // origin -> user, password
std::map<std::string, LocalRequestHandler> g_local;  // origin -> handler (set at startup, then only read)

std::string origin_of(const std::string& url) {
    Url u;
    return parse_url(url, u) ? lower(u.origin()) : std::string();
}

// Logs the address without its query string, which can hold search terms.
std::string log_target(const std::string& url) {
    Url u;
    if (!parse_url(url, u)) return "(invalid)";
    return u.origin() + url_path_only(u.path).substr(0, 120);
}

thread_local bool t_skip_verification = false;


HttpResponse perform(const HttpRequest& request, const BodySink& sink, uint64_t* total_out,
                     const HeadHook& head = nullptr) {
    HttpResponse response;
    std::string url = request.url;
    const std::string auth_origin = request.auth_origin.empty() ? url : request.auth_origin;
    std::string method = request.method;
    std::string body = request.body;
    for (int hop = 0; hop <= request.max_redirects; ++hop) {
        Url parsed;
        if (!parse_url(url, parsed)) {
            response.error = "The address is not a valid http:// or https:// URL.";
            return response;
        }
        auto headers = request.headers;
        bool has_agent = false;
        for (const auto& header : headers) if (iequals(header.first, "User-Agent")) has_agent = true;
        if (!has_agent) headers.emplace_back("User-Agent", "FPKGi-Plus/1.2 (https://github.com/jw-notes)");
        headers.emplace_back("Accept-Encoding", "identity");
        // Compute for each redirect hop, never forward a session to another host.
        const std::string archive_cookie = request.verify_certificate ? archive_cookie_for(url) : "";
        if (!archive_cookie.empty() && !request.no_auto_auth) headers.emplace_back("Cookie", archive_cookie);
        if (!request.username.empty()) {
            if (same_origin(url, auth_origin))
                headers.emplace_back("Authorization", "Basic " + base64_encode(request.username + ":" + request.password));
        } else if (!request.no_auto_auth) {
            pthread_mutex_lock(&g_auth_lock);
            auto found = g_credentials.find(origin_of(url));
            if (found != g_credentials.end())
                headers.emplace_back("Authorization",
                                     "Basic " + base64_encode(found->second.first + ":" + found->second.second));
            pthread_mutex_unlock(&g_auth_lock);
        }

        HttpRawResult raw;
        auto local = g_local.find(origin_of(url));
        if (local != g_local.end()) {
            // Served in-process: not logged (a page and its pictures are many requests).
            std::string data;
            local->second(url, headers, raw, data);
            raw.content_length = data.size();
            const bool redirect = raw.status >= 300 && raw.status < 400 && !raw.location.empty();
            if (!redirect && method != "HEAD" && (!head || head(raw)) && !data.empty()) sink(data.data(), data.size());
        } else {
            LOGD("HTTP %s %s", method.c_str(), log_target(url).c_str());
            t_skip_verification = !request.verify_certificate;
            raw = http_perform(method, url, body, headers, request.timeout_seconds, sink, head);
            t_skip_verification = false;
        }
        response.status = raw.status;
        response.content_type = raw.content_type;
        response.content_range = raw.content_range;
        response.error = raw.error;
        response.final_url = url;
        for (auto& c : raw.set_cookies) response.set_cookies.push_back(std::move(c));
        if (total_out) *total_out = raw.content_length;
        response.content_length = raw.content_length;
        if (local == g_local.end()) {
            // A failure is always worth a line (with what was asked, which
            // the detail line above only gives in debug logging).
            if (raw.status < 0 || raw.status >= 400)
                LOG("HTTP %s %s: status %d %s", method.c_str(), log_target(url).c_str(), raw.status, raw.error.c_str());
            else
                LOGD("HTTP status %d %s", raw.status, raw.error.c_str());
        }
        if (request.follow_redirects &&
            (raw.status == 301 || raw.status == 302 || raw.status == 303 || raw.status == 307 || raw.status == 308) &&
            !raw.location.empty()) {
            url = resolve_url(url, raw.location);
            if (raw.status == 303 || ((raw.status == 301 || raw.status == 302) && method == "POST")) {
                method = "GET";
                body.clear();
            }
            continue;
        }
        return response;
    }
    response.error = "Too many redirects.";
    response.status = -1;
    return response;
}

}  // namespace

void http_register_local(const std::string& origin, LocalHandler handler) {
    g_local[origin_of(origin)] = [handler = std::move(handler)](const std::string& url, const RequestHeaders&,
                                                                HttpRawResult& head, std::string& body) {
        handler(url, head, body);
    };
}

void http_register_local_request(const std::string& origin, LocalRequestHandler handler) {
    g_local[origin_of(origin)] = std::move(handler);
}

void http_set_credentials(const std::string& origin_url, const std::string& user, const std::string& password) {
    const std::string origin = origin_of(origin_url);
    if (origin.empty()) return;
    pthread_mutex_lock(&g_auth_lock);
    if (user.empty()) g_credentials.erase(origin);
    else g_credentials[origin] = {user, password};
    pthread_mutex_unlock(&g_auth_lock);
}

HttpResponse http_request(const HttpRequest& request) {
    std::string body;
    bool too_big = false;
    HttpResponse response = perform(request, [&](const char* data, size_t size) {
        if (body.size() + size > request.max_bytes) {
            too_big = true;
            return false;
        }
        body.append(data, size);
        return true;
    }, nullptr);
    if (too_big) {
        response.status = -1;
        response.error = "The response is larger than " + format_bytes(request.max_bytes) + ".";
        return response;
    }
    response.body = std::move(body);
    return response;
}

namespace {

bool has_header(const HttpRequest& request, const char* name) {
    for (const auto& header : request.headers)
        if (iequals(header.first, name)) return true;
    return false;
}

// A validator is sent back to the server in a header and kept in a file:
// anything that could not have come from one header line is dropped.
std::string clean_validator(const std::string& value) {
    if (value.empty() || value.size() > 512) return "";
    for (unsigned char c : value)
        if (c < 0x20 || c == 0x7F) return "";
    return value;
}

RemoteValidators validators_of(const HttpRawResult& raw) {
    return {clean_validator(raw.etag), clean_validator(raw.last_modified)};
}

std::string strong_part(const std::string& etag) { return starts_with(etag, "W/") ? etag.substr(2) : etag; }

// The notes beside a partial file written by one connection (`path`.part.meta).
struct PartialNotes {
    bool known = false;  // false: none (a partial file from before they were kept)
    RemoteValidators validators;
    uint64_t total = 0;  // 0: the server did not say
};

PartialNotes read_notes(const std::string& notes_path) {
    PartialNotes notes;
    std::string text;
    bool ok = false;
    if (!read_file(notes_path, text, 64 * 1024)) return notes;
    const Json saved = Json::parse(text, &ok);
    if (!ok || !saved.is_object() || saved["version"].integer() != 1) return notes;
    const int64_t total = saved["total"].integer();
    if (total < 0) return notes;
    notes.known = true;
    notes.total = (uint64_t)total;
    notes.validators = {clean_validator(saved["etag"].str()), clean_validator(saved["last_modified"].str())};
    return notes;
}

bool write_notes(const std::string& notes_path, const RemoteValidators& validators, uint64_t total) {
    Json root = Json::object();
    root.set("version", 1);
    root.set("total", (int64_t)total);
    root.set("etag", validators.etag);
    root.set("last_modified", validators.last_modified);
    return write_file_atomic(notes_path, root.dump());
}

}  // namespace

std::string RemoteValidators::if_range() const {
    if (!etag.empty() && !starts_with(etag, "W/")) return etag;
    return last_modified;
}

RemoteMatch compare_validators(const RemoteValidators& saved, const RemoteValidators& current) {
    if (!saved.etag.empty() && !current.etag.empty())
        return strong_part(saved.etag) == strong_part(current.etag) ? RemoteMatch::Same : RemoteMatch::Changed;
    if (!saved.last_modified.empty() && !current.last_modified.empty())
        return saved.last_modified == current.last_modified ? RemoteMatch::Same : RemoteMatch::Changed;
    return RemoteMatch::Unverifiable;
}

std::vector<std::string> http_partial_files(const std::string& path) {
    return {path + ".part", path + ".part.chunks", path + ".part.meta", path + ".resume", path + ".resume.chunks"};
}

HttpResponse http_download(const HttpRequest& request, const std::string& path, const Progress& progress,
                           bool resume, const SpaceCheck& space) {
    const std::string temp = path + ".part", notes_path = temp + ".meta";
    // A caller that asks for a range of its own (a header probe) is not
    // downloading a file to be continued: no notes are kept for it.
    const bool track = resume && !has_header(request, "Range");
    bool remote_changed = false;
    HttpResponse response;
    // A second pass only after the first found the saved bytes belong to
    // another version of the file and removed them.
    for (int attempt = 0; attempt < 2; ++attempt) {
        const uint64_t have = resume && file_exists(temp) ? file_size(temp) : 0;
        PartialNotes saved;
        if (track) {
            if (have > 0) saved = read_notes(notes_path);
            else if (file_exists(notes_path)) remove_file(notes_path);  // notes without their bytes
        }
        HttpRequest req = request;
        const std::string condition = have > 0 && saved.known ? saved.validators.if_range() : std::string();
        if (have > 0) {
            req.headers.emplace_back("Range", "bytes=" + std::to_string(have) + "-");
            // The server sends the rest only if the file is still that version;
            // otherwise all of it (200), which is saved from the start.
            if (!condition.empty()) req.headers.emplace_back("If-Range", condition);
        }
        FILE* out = nullptr;
        uint64_t done = 0, total = 0;
        bool write_failed = false, cancelled = false, open_failed = false, bad_range = false;
        bool changed = false, no_space = false;
        std::string space_error;
        // The file is opened once the answer is known: appended to when the
        // server sends the rest (206), started again when it sends everything.
        auto head = [&](const HttpRawResult& raw) {
            if (raw.status < 200 || raw.status >= 300) return true;  // an error page: read, not kept
            const RemoteValidators current = validators_of(raw);
            if (raw.status == 206 && have > 0) {
                unsigned long long start = 0, end = 0, full = 0;
                if (sscanf(raw.content_range.c_str(), "bytes %llu-%llu/%llu", &start, &end, &full) != 3 ||
                    start != have || end < start || end >= full || end + 1 != full ||
                    (raw.content_length && raw.content_length != end - start + 1)) {
                    bad_range = true;
                    return false;
                }
                // A server may answer a range without honouring If-Range (or
                // none could be sent): the answer itself is checked as well.
                if (track && saved.known &&
                    ((saved.total && saved.total != full) ||
                     compare_validators(saved.validators, current) == RemoteMatch::Changed)) {
                    changed = true;
                    return false;
                }
                if (space && !space(full - have, full, space_error)) {
                    no_space = true;
                    return false;
                }
                if (track && (!saved.known || (!saved.validators.any() && current.any()))) {
                    // Nothing to compare with: continued as it always was,
                    // and what the server says now is noted for the next time.
                    LOG("download: %s; continuing on its size alone",
                        saved.known ? "the partial file's notes hold no validators"
                                    : "the partial file predates remote-file validation");
                    write_notes(notes_path, current, full);
                } else if (track && !current.any()) {
                    LOGD("download: the server gives no ETag or Last-Modified; continuing on its size alone");
                }
                out = open_file(temp, "ab");
                done = have;
                total = full;
            } else {
                // All of the file: a first attempt, a server without ranges,
                // or one saying (If-Range) that the saved bytes are out of date.
                const uint64_t length = raw.content_length;
                // The partial file is replaced, so its space counts as free.
                if (space && !space(length > have ? length - have : 0, length, space_error)) {
                    no_space = true;
                    return false;
                }
                if (have > 0) {
                    const RemoteMatch match = saved.known ? compare_validators(saved.validators, current)
                                                          : RemoteMatch::Unverifiable;
                    if (match == RemoteMatch::Changed || (match == RemoteMatch::Unverifiable && !condition.empty()) ||
                        (saved.known && saved.total && length && saved.total != length))
                        remote_changed = true;
                    LOG("download: %s; starting again from the beginning",
                        remote_changed ? "the file changed on the server" : "the server sent the whole file");
                }
                if (track) {
                    // Old notes and old bytes go before the new notes are
                    // written: a crash in between leaves notes without bytes
                    // (dropped next time), never old bytes under new notes.
                    if (saved.known) remove_file(notes_path);
                    if (have > 0) remove_file(temp);
                    write_notes(notes_path, current, length);
                }
                out = open_file(temp, "wb");
                done = 0;
                total = length;
            }
            if (!out) open_failed = true;
            else setvbuf(out, nullptr, _IOFBF, 1 << 20);  // fewer, larger writes to the disk
            return out != nullptr;
        };
        response = perform(req, [&](const char* data, size_t size) {
            if (!out) return true;
            if (fwrite(data, 1, size, out) != size) {
                write_failed = true;
                return false;
            }
            done += size;
            if (progress && !progress(done, total)) {
                cancelled = true;
                return false;
            }
            return true;
        }, nullptr, head);
        const bool closed = !out || fclose(out) == 0;
        if (changed && attempt == 0) {
            LOG("download: the file changed on the server; the partial file was discarded and it starts again");
            remove_file(notes_path);
            remove_file(temp);
            remote_changed = true;
            continue;
        }
        if (bad_range) {
            response.status = -1;
            response.error = "The server sent an invalid resume range. The partial file was kept.";
        } else if (changed) {
            response.status = -1;
            response.error = "The file on the server keeps changing. Try again later.";
        } else if (no_space) {
            response.status = -1;
            response.error = space_error.empty() ? "Not enough free space for this download." : space_error;
            response.insufficient_space = true;
        } else if (open_failed) {
            response.status = -1;
            response.error = "Could not create " + path + ".";
        } else if (write_failed || !closed) {
            response.status = -1;
            response.error = "Could not write the file (is the storage full?).";
        } else if (cancelled) {
            response.status = -1;
            response.error = "Cancelled.";
        } else if (response.ok() && total && done != total) {
            response.status = -1;
            response.error = "The download ended early.";
        }
        if (response.ok()) {
            if (!rename_file(temp, path)) {
                response.status = -1;
                response.error = "Could not save " + path + ".";
            } else if (track) {
                remove_file(notes_path);
            }
        }
        if (!response.ok() && !no_space && (!resume || open_failed)) remove_file(temp);
        if (open_failed && track) remove_file(notes_path);
        break;
    }
    response.remote_changed = remote_changed;
    return response;
}

namespace {
uint64_t g_parallel_chunk = 64ull * 1024 * 1024;

// What the .chunks notes say is saved of each range, never more than the
// range holds or than the partial file is long.
bool read_chunk_notes(const Json& saved, uint64_t disk_size, std::vector<uint64_t>& done) {
    const int64_t total = saved["total"].integer(), chunk = saved["chunk"].integer();
    if (!saved.is_object() || total <= 0 || chunk <= 0) return false;
    const uint64_t count = ((uint64_t)total + (uint64_t)chunk - 1) / (uint64_t)chunk;
    if (count > (1u << 20) || saved["done"].size() != count) return false;
    done.assign((size_t)count, 0);
    for (size_t i = 0; i < done.size(); ++i) {
        const uint64_t offset = (uint64_t)i * (uint64_t)chunk;
        const uint64_t size = std::min<uint64_t>((uint64_t)chunk, (uint64_t)total - offset);
        const uint64_t available = disk_size > offset ? disk_size - offset : 0;
        const int64_t recorded = saved["done"][i].integer();
        done[i] = std::min<uint64_t>(recorded > 0 ? (uint64_t)recorded : 0, std::min(size, available));
    }
    return true;
}

HttpResponse parallel_once(const HttpRequest& request, const std::string& path, const Progress& progress,
                           int connections, bool sequential_only, const SpaceCheck& space, bool& remote_changed,
                           bool& changed_midway) {
    const std::string temp = path + ".part", state_path = temp + ".chunks", notes_path = temp + ".meta";
    bool have_state = file_exists(state_path);
    if (have_state && !file_exists(temp)) {  // the partial file was deleted: its notes mean nothing
        remove_file(state_path);
        have_state = false;
    }
    const auto sequential = [&] {
        if (have_state) {  // a file with holes cannot be continued from its end
            remove_file(state_path);
            remove_file(temp);
            remove_file(notes_path);
        }
        return http_download(request, path, progress, true, space);
    };
    if (sequential_only || (connections <= 1 && !have_state)) return sequential();
    connections = std::max(1, std::min(connections, 8));

    // One byte, to learn the size, whether ranges work, where redirects end
    // and which version of the file the server has now.
    uint64_t total = 0;
    std::string direct_url;
    RemoteValidators validators;
    {
        HttpRequest probe = request;
        probe.headers.emplace_back("Range", "bytes=0-0");
        probe.timeout_seconds = 20;
        std::string range;
        const HttpResponse r = perform(probe, [](const char*, size_t) { return true; }, nullptr,
                                       [&](const HttpRawResult& raw) {
                                           range = raw.content_range;
                                           validators = validators_of(raw);
                                           return false;
                                       });
        unsigned long long a = 0, b = 0, full = 0;
        if (r.status != 206 || sscanf(range.c_str(), "bytes %llu-%llu/%llu", &a, &b, &full) != 3 || full == 0) {
            if (r.status < 0 || r.status >= 400) {  // the real reason (401, 403, no network), not a second try
                HttpResponse failed = r;
                if (failed.error.empty() && failed.status < 0) failed.error = "No response from the server.";
                return failed;
            }
            return sequential();
        }
        total = full;
        direct_url = r.final_url;
    }
    const uint64_t chunk = g_parallel_chunk;
    if (total < 2 * chunk && !have_state) return sequential();

    // What is already there, if it is the same version of the file.
    const size_t count = (size_t)((total + chunk - 1) / chunk);
    std::vector<uint64_t> done(count, 0);
    const auto size_of = [&](size_t i) { return std::min<uint64_t>(chunk, total - (uint64_t)i * chunk); };
    bool fresh = true;
    if (have_state) {
        std::string text;
        bool ok = false;
        const Json saved = read_file(state_path, text, 4 << 20) ? Json::parse(text, &ok) : Json();
        const bool readable = ok && saved.is_object();
        const bool same_size = readable && (uint64_t)saved["total"].integer() == total;
        // Notes from before validators were kept have no version.
        const bool legacy = readable && saved["version"].integer() < 2;
        const RemoteValidators noted{clean_validator(saved["etag"].str()), clean_validator(saved["last_modified"].str())};
        const RemoteMatch match = legacy ? RemoteMatch::Unverifiable : compare_validators(noted, validators);
        std::vector<uint64_t> restored;
        if (same_size && (uint64_t)saved["chunk"].integer() == chunk && match != RemoteMatch::Changed &&
            read_chunk_notes(saved, file_size(temp), restored) && restored.size() == count) {
            done = restored;
            fresh = false;
            if (legacy) LOG("download: the resume notes predate remote-file validation; continuing on the size alone");
            else if (match == RemoteMatch::Unverifiable)
                LOG("download: the server gives no ETag or Last-Modified; continuing on the size alone");
        } else {
            if (readable && (!same_size || match == RemoteMatch::Changed)) {
                remote_changed = true;
                LOG("download: the file changed on the server; the partial file was discarded and it starts again");
            }
            remove_file(state_path);
            remove_file(temp);
        }
    } else if (file_exists(temp)) {
        // Started over one connection: the file is whole up to its end, if
        // it is still the file those bytes came from.
        const PartialNotes notes = read_notes(notes_path);
        if (notes.known && ((notes.total && notes.total != total) ||
                            compare_validators(notes.validators, validators) == RemoteMatch::Changed)) {
            remote_changed = true;
            LOG("download: the file changed on the server; the partial file was discarded and it starts again");
            remove_file(temp);
        } else {
            if (!notes.known) LOG("download: the partial file predates remote-file validation; continuing on its size alone");
            uint64_t have = std::min<uint64_t>(file_size(temp), total);
            for (size_t i = 0; i < count && have > 0; ++i) {
                done[i] = std::min<uint64_t>(have, size_of(i));
                have -= done[i];
            }
            fresh = false;
        }
    }
    if (fresh || !file_exists(temp)) std::fill(done.begin(), done.end(), 0);
    if (space) {
        // What every connection together has saved, not the file's length:
        // a file written out of order is as long as its furthest range.
        uint64_t saved_bytes = 0;
        for (size_t i = 0; i < count; ++i) saved_bytes += done[i];
        std::string error;
        if (!space(total - std::min(total, saved_bytes), total, error)) {
            HttpResponse r;
            r.error = error.empty() ? "Not enough free space for this download." : error;
            r.insufficient_space = true;
            return r;
        }
    }
    if (fresh || !file_exists(temp)) {
        FILE* created = fopen(temp.c_str(), "wb");
        if (!created) {
            HttpResponse r;
            r.error = "Could not create " + path + ".";
            return r;
        }
        fclose(created);
    }

    std::mutex lock;  // done, taken, failure
    std::vector<char> taken(count, 0);
    std::atomic<bool> stop{false}, changed{false};
    std::atomic<uint64_t> received{0};
    for (size_t i = 0; i < count; ++i) received += done[i];
    HttpResponse failure;
    bool failed = false, write_failed = false;
    const std::string condition = validators.if_range();

    // `margin`: bytes that may still be in a connection's write buffer are not
    // counted, in case the app is killed before they reach the disk.
    const auto save_state = [&](uint64_t margin) {
        Json list = Json::array();
        {
            std::lock_guard<std::mutex> l(lock);
            for (size_t i = 0; i < count; ++i)
                list.push((int64_t)(done[i] >= size_of(i) && !taken[i] ? done[i] : done[i] > margin ? done[i] - margin : 0));
        }
        Json root = Json::object();
        root.set("version", 2);
        root.set("total", (int64_t)total);
        root.set("chunk", (int64_t)chunk);
        root.set("etag", validators.etag);
        root.set("last_modified", validators.last_modified);
        root.set("done", list);
        write_file_atomic(state_path, root.dump());
    };
    save_state(0);
    // The .chunks notes now say which version this is.
    if (file_exists(notes_path)) remove_file(notes_path);

    auto worker = [&] {
        FILE* out = fopen(temp.c_str(), "r+b");
        if (!out) {
            std::lock_guard<std::mutex> l(lock);
            write_failed = true;
            stop = true;
            return;
        }
        setvbuf(out, nullptr, _IOFBF, 1 << 20);
        bool use_direct = !direct_url.empty();
        int failures = 0;
        while (!stop) {
            size_t i = count;
            uint64_t from = 0;
            {
                std::lock_guard<std::mutex> l(lock);
                for (size_t k = 0; k < count; ++k)
                    if (!taken[k] && done[k] < size_of(k)) { i = k; break; }
                if (i == count) break;
                taken[i] = 1;
                from = done[i];
            }
            const uint64_t start = (uint64_t)i * chunk + from, end = (uint64_t)i * chunk + size_of(i) - 1;
            HttpRequest r = request;
            if (use_direct) r.url = direct_url;  // skips the redirect of every range
            r.headers.emplace_back("Range", "bytes=" + std::to_string(start) + "-" + std::to_string(end));
            if (!condition.empty()) r.headers.emplace_back("If-Range", condition);
            bool accepted = false, disk = true;
            const HttpResponse got = perform(r, [&](const char* data, size_t size) {
                if (!accepted) return true;  // an error page: read, not kept
                if (fwrite(data, 1, size, out) != size) { disk = false; return false; }
                {
                    std::lock_guard<std::mutex> l(lock);
                    done[i] += size;
                }
                received += size;
                return !stop.load();
            }, nullptr, [&](const HttpRawResult& raw) {
                unsigned long long a = 0, b = 0, full = 0;
                const bool ranged = raw.status == 206 &&
                                    sscanf(raw.content_range.c_str(), "bytes %llu-%llu/%llu", &a, &b, &full) == 3;
                // The whole file in answer to If-Range, another size or other
                // validators: the server's file is no longer the one the saved
                // ranges came from. Nothing of this answer is kept.
                if ((raw.status == 200 && !condition.empty()) ||
                    (ranged && (full != total ||
                                compare_validators(validators, validators_of(raw)) == RemoteMatch::Changed))) {
                    changed = true;
                    stop = true;
                    return false;
                }
                accepted = ranged && a == start && b == end && fseeko(out, (off_t)start, SEEK_SET) == 0;
                return true;
            });
            const bool flushed = fflush(out) == 0;
            std::unique_lock<std::mutex> l(lock);
            taken[i] = 0;
            if (!disk || !flushed) {
                write_failed = true;
                stop = true;
                break;
            }
            if (done[i] >= size_of(i)) { failures = 0; continue; }
            if (stop) break;
            // A range that failed or ended early is asked for again from where it
            // stopped: first through the original address (a mirror's link can expire).
            if (use_direct && !accepted) use_direct = false;
            if (++failures > 5) {
                if (!failed) {
                    failed = true;
                    failure = got;
                    if (failure.ok() || (failure.status == 206 && failure.error.empty())) {
                        failure.status = -1;
                        failure.error = "The download ended early.";
                    }
                }
                stop = true;
                break;
            }
            l.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(400 * failures));
        }
        fclose(out);
    };
    // PS4 is built without C++ exceptions. std::thread terminates the app if
    // the OS cannot create a worker; handle that failure explicitly on resume.
    std::vector<pthread_t> threads;
    for (int t = 0; t < connections; ++t) {
        pthread_t thread;
        const int rc = pthread_create(&thread, nullptr, [](void* context) -> void* {
            (*static_cast<decltype(worker)*>(context))();
            return nullptr;
        }, &worker);
        if (rc != 0) {
            LOG("download: worker creation failed (%d), continuing with %zu connections", rc, threads.size());
            break;
        }
        threads.push_back(thread);
    }
    if (threads.empty()) {
        HttpResponse response;
        response.error = "Could not start a download worker. The partial download was kept; try again.";
        return response;
    }
    LOG("download: %s with %zu connections, %llu of %llu bytes saved", fresh ? "starting" : "resuming",
        threads.size(), (unsigned long long)received.load(), (unsigned long long)total);

    bool cancelled = false;
    int ticks = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (progress && !stop && !progress(received.load(), total)) {
            cancelled = true;
            stop = true;
        }
        bool complete = true;
        {
            std::lock_guard<std::mutex> l(lock);
            for (size_t i = 0; i < count && complete; ++i) complete = done[i] >= size_of(i);
        }
        if (complete || stop) break;
        if (++ticks % 50 == 0) save_state(2 << 20);
    }
    stop = true;
    for (auto& t : threads) pthread_join(t, nullptr);

    HttpResponse response;
    response.final_url = direct_url;
    response.content_length = total;
    if (changed) {
        // Ranges of two versions may already be in the file: none of it is kept.
        LOG("download: the file changed on the server during the download; the partial file was discarded");
        remove_file(state_path);
        remove_file(temp);
        changed_midway = true;
        response.error = "The file changed on the server during the download.";
        return response;
    }
    bool complete = true;
    for (size_t i = 0; i < count && complete; ++i) complete = done[i] >= size_of(i);
    if (complete && !write_failed) {
        remove_file(state_path);
        if (rename_file(temp, path)) {
            response.status = 200;
            if (progress) progress(total, total);
        } else {
            response.error = "Could not save " + path + ".";
        }
        return response;
    }
    save_state(0);
    if (write_failed) response.error = "Could not write the file (is the storage full?).";
    else if (cancelled) response.error = "Cancelled.";
    else if (failed) response = failure;
    else response.error = "The download ended early.";
    if (response.status >= 200 && response.status < 300) response.status = -1;
    return response;
}
}  // namespace

void http_set_parallel_chunk(uint64_t bytes) { g_parallel_chunk = bytes ? bytes : 64ull * 1024 * 1024; }

HttpResponse http_download_parallel(const HttpRequest& request, const std::string& path, const Progress& progress,
                                    int connections, bool sequential_only, const SpaceCheck& space) {
    bool remote_changed = false, changed_midway = false;
    HttpResponse response =
        parallel_once(request, path, progress, connections, sequential_only, space, remote_changed, changed_midway);
    if (changed_midway) {
        // Once more, from nothing, against the version the server has now.
        remote_changed = true;
        changed_midway = false;
        response = parallel_once(request, path, progress, connections, sequential_only, space, remote_changed,
                                 changed_midway);
        if (changed_midway) {
            response = HttpResponse();
            response.error = "The file on the server keeps changing. Try again later.";
        }
    }
    response.remote_changed = response.remote_changed || remote_changed;
    return response;
}

uint64_t http_partial_bytes(const std::string& path) {
    const std::string temp = path + ".part", state_path = temp + ".chunks";
    if (file_exists(temp)) {
        const uint64_t disk_size = file_size(temp);
        std::string text;
        bool ok = false;
        std::vector<uint64_t> done;
        if (file_exists(state_path) && read_file(state_path, text, 4 << 20)) {
            const Json saved = Json::parse(text, &ok);
            if (ok && read_chunk_notes(saved, disk_size, done)) {
                uint64_t sum = 0;
                for (uint64_t bytes : done) sum += bytes;
                return sum;
            }
            return 0;  // unreadable notes: the next attempt starts again
        }
        return disk_size;
    }
    if (file_exists(path + ".resume")) return file_size(path + ".resume");
    return 0;
}

std::string describe_failure(const HttpResponse& r) {
    if (r.status == 401 && (origin_of(r.final_url) == "https://archive.org" ||
                           ends_with(origin_of(r.final_url), ".archive.org")))
        return archive_signed_in() ? "Archive.org refused this session (401). Sign in again in Settings. Your account may not have access to this file." :
               "Archive.org requires sign-in (401). Open Settings > Archive.org account, sign in, then retry the download.";
    if (r.status == 401) return "The server asked for a user name and password, or rejected the ones in Settings.";
    if (r.status == 403) return "The server refused access (403). Check the account's permissions.";
    if (r.status == 404) return "The server has no such page (404).";
    if (r.status >= 500) return "The server had an internal error (" + std::to_string(r.status) + ").";
    if (r.status > 0 && !r.ok()) return "The server answered with status " + std::to_string(r.status) + ".";
    if (!r.error.empty()) return r.error;
    return "No response from the server. Check the address and the network connection.";
}

bool http_skip_verification() { return t_skip_verification; }

}  // namespace lib
