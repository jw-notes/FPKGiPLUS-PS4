// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// HTTP client. Redirects are followed here (not by the console's library) so
// the final address of a page is known: Kiwix articles use links relative to
// it. Basic-auth credentials are only ever sent to the origin they belong to.
#include <stdint.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace lib {

struct HttpRequest {
    std::string url;
    std::string method = "GET";
    std::string body;
    std::string username, password;  // HTTP Basic, for `auth_origin` only
    std::string auth_origin;         // defaults to the origin of `url`
    std::vector<std::pair<std::string, std::string>> headers;
    uint64_t max_bytes = 48ull * 1024 * 1024;
    int timeout_seconds = 20;
    int max_redirects = 8;
    bool follow_redirects = true;  // false: return a 3xx as-is (for login)
    // Do not attach the per-origin stored Basic credentials (used for session/
    // cookie requests, which carry their own auth and must not trip limiters).
    bool no_auto_auth = false;
    // false: the server's certificate is not checked. Only for content whose
    // integrity is checked otherwise (the software update, against the
    // SHA-256 that api.github.com gives over a checked connection).
    bool verify_certificate = true;
};

struct HttpResponse {
    int status = -1;  // -1: no response (connection, DNS, TLS failure)
    std::string body;
    std::string final_url;
    std::string content_type;
    std::string content_range;
    std::string error;  // human-readable, for the UI
    std::vector<std::string> set_cookies;  // raw Set-Cookie header values
    uint64_t content_length = 0;           // as the server gave it (0: not given)
    // Downloads: a partial file was discarded and the file fetched from its
    // start, because the server's copy no longer matched the saved bytes.
    bool remote_changed = false;
    // Downloads: stopped by the SpaceCheck before anything was written.
    bool insufficient_space = false;

    bool ok() const { return status >= 200 && status < 300 && error.empty(); }
};

// Called with each chunk of the body; return false to stop the transfer.
using BodySink = std::function<bool(const char* data, size_t size)>;
// Called with bytes so far and the total (0 if the server does not say).
using Progress = std::function<bool(uint64_t done, uint64_t total)>;
// Called by a download once the server has said how large the file is and
// before anything is written: `remaining` is what this attempt still has to
// store (the total less the bytes already saved, whichever connections saved
// them), `total` the whole file (0: the server did not say). Returning false
// stops the download with `error`; the partial file and its notes are kept.
using SpaceCheck = std::function<bool(uint64_t remaining, uint64_t total, std::string& error)>;

// What identifies one version of a remote file, kept beside a partial
// download so that it is only continued from the same version.
struct RemoteValidators {
    std::string etag, last_modified;
    bool any() const { return !etag.empty() || !last_modified.empty(); }
    // The value for an If-Range header: a strong ETag, else Last-Modified.
    // Empty when the server gave neither (a weak ETag cannot be used there).
    std::string if_range() const;
};
enum class RemoteMatch { Same, Changed, Unverifiable };
// Compares what was saved with what the server says now. ETags decide when
// both sides have one (compared weakly), else Last-Modified; with neither on
// one side nothing can be said.
RemoteMatch compare_validators(const RemoteValidators& saved, const RemoteValidators& current);

bool http_init();
void http_shutdown();
// Credentials sent (HTTP Basic) to every request for this origin, unless the
// request carries its own. Set from Settings; an empty user removes them.
void http_set_credentials(const std::string& origin_url, const std::string& user, const std::string& password);
// Accept self-signed certificates (a setting: many home servers use them).
void http_set_allow_insecure(bool allow);

HttpResponse http_request(const HttpRequest& request);
// Streams the body to `path` (via `path`.part, renamed on success). With
// `resume`, a .part file left by an earlier attempt is continued (HTTP Range)
// when the server allows it, and kept when this attempt fails or is cancelled.
//
// A resumed file is only continued from the same remote version. The server's
// ETag / Last-Modified and size are kept in `path`.part.meta; the next attempt
// sends them as If-Range and checks the answer. A changed file is fetched
// again from its start (HttpResponse::remote_changed), never appended to. A
// server that gives no validators is continued on its size alone, and a
// partial file from before these notes existed ("legacy") is continued as it
// always was and gets its notes from that answer.
HttpResponse http_download(const HttpRequest& request, const std::string& path, const Progress& progress,
                           bool resume = false, const SpaceCheck& space = nullptr);
// The same file over several connections at once (each asks for its own
// range), which is much faster from servers that limit the speed of one
// connection (archive.org). Progress is kept beside the partial file
// (`path`.part.chunks), so a stopped download continues where each connection
// was. Falls back to http_download when the server does not do ranges, the
// file is small or `connections` is 1; `sequential_only` forces that (storage
// that cannot be written out of order: a drive reached through FTP).
// The validators are kept in the .chunks notes and every range is asked for
// with If-Range, so a file that changes on the server (before a resume or in
// the middle of a download) is started again instead of being mixed.
HttpResponse http_download_parallel(const HttpRequest& request, const std::string& path, const Progress& progress,
                                    int connections, bool sequential_only = false, const SpaceCheck& space = nullptr);
// For tests: the size of one range (64 MB otherwise).
void http_set_parallel_chunk(uint64_t bytes);
// Bytes of `path` already saved by an unfinished download: the sum of the
// ranges in its .part.chunks notes (a file written out of order has holes, so
// its length says nothing), else the length of its .part / .resume file.
uint64_t http_partial_bytes(const std::string& path);
// The files a download keeps beside `path` while unfinished.
std::vector<std::string> http_partial_files(const std::string& path);
// Explains a failed response in a sentence fit for an error screen.
std::string describe_failure(const HttpResponse& response);

// ---- implemented by the platform backend (http_ps4.cpp / http_host.cpp) ----
// Performs one request without following redirects. `sink` receives the body
// of any response that is not a 3xx; before it, `head` (optional) sees the
// status and headers, and returning false skips the body.
struct HttpRawResult {
    int status = -1;
    std::string location, content_type, error, content_range;
    std::string etag, last_modified;  // as sent ("" when absent)
    uint64_t content_length = 0;
    std::vector<std::string> set_cookies;
};
using HeadHook = std::function<bool(const HttpRawResult& head)>;
HttpRawResult http_perform(const std::string& method, const std::string& url, const std::string& body,
                           const std::vector<std::pair<std::string, std::string>>& headers, int timeout_seconds,
                           const BodySink& sink, const HeadHook& head = nullptr);

// Whether the request being performed on this thread skips the certificate
// check (HttpRequest::verify_certificate), for the backends.
bool http_skip_verification();

// Requests to `origin` (e.g. "http://local.app") are answered in-process
// by `handler` instead of the network: local content can be served
// the way kiwix-serve serves its collections. The handler fills the status,
// type and Location of `head` and the body; it runs on the calling thread.
using LocalHandler = std::function<void(const std::string& url, HttpRawResult& head, std::string& body)>;
void http_register_local(const std::string& origin, LocalHandler handler);
// The same, for a handler that needs the request's headers (Range, If-Range,
// Authorization) as they would have been sent.
using RequestHeaders = std::vector<std::pair<std::string, std::string>>;
using LocalRequestHandler =
    std::function<void(const std::string& url, const RequestHeaders& headers, HttpRawResult& head, std::string& body)>;
void http_register_local_request(const std::string& origin, LocalRequestHandler handler);

}  // namespace lib
