#include "net/archive.h"
#include <mutex>
#include "net/http.h"
#include "net/url.h"
#include "core/fs.h"
#include "core/text.h"
#include "core/log.h"

namespace lib {
namespace {
std::mutex session_lock;
std::string session_cookie;
std::string session_path;  // empty: the session is not kept between runs
bool safe_cookie(const std::string& value) {
    if (value.empty() || value.size() > 16384) return false;
    for (unsigned char c : value) if (c <= 0x20 || c >= 0x7f || c == ';' || c == ',' || c == '"' || c == '\\') return false;
    return true;
}
// Archive.org may append cookie attributes ("value; expires=...; path=/").
std::string cookie_value(const std::string& raw) {
    return trim(raw.substr(0, raw.find(';')));
}
// With the lock held. False when either value is unusable.
bool set_session(const std::string& user, const std::string& sig) {
    if (!safe_cookie(user) || !safe_cookie(sig)) return false;
    session_cookie = "logged-in-user=" + user + "; logged-in-sig=" + sig;
    return true;
}
}
void archive_sign_out() {
    std::lock_guard<std::mutex> lock(session_lock);
    session_cookie.clear();
    if (!session_path.empty()) remove_file(session_path);
}
bool archive_restore_session(const std::string& path) {
    std::lock_guard<std::mutex> lock(session_lock);
    session_path = path;
    std::string text;
    if (path.empty() || !read_file(path, text, 64 * 1024)) return false;
    bool ok = false;
    const Json saved = Json::parse(text, &ok);
    if (!ok || !set_session(saved["user"].str(), saved["sig"].str())) {
        LOG("archive sign-in: the saved session is unreadable and was removed");
        remove_file(path);
        return false;
    }
    LOG("archive sign-in: saved session restored");
    return true;
}
bool archive_signed_in() {
    std::lock_guard<std::mutex> lock(session_lock);
    return !session_cookie.empty();
}
std::string archive_cookie_for(const std::string& url) {
    Url u;
    if (!parse_url(url, u) || u.scheme != "https" || u.port != 443) return "";
    const std::string host = lower(u.host);
    if (host != "archive.org" && !ends_with(host, ".archive.org")) return "";
    std::lock_guard<std::mutex> lock(session_lock);
    return session_cookie;
}
std::string archive_accept_login(const Json& response) {
    if (!response["success"].boolean()) {
        std::string reason = response["values"]["reason"].str();
        if (reason.empty()) reason = response["error"].str();
        if (reason == "account_bad_password" || reason == "account_not_found")
            return "Archive.org rejected the email or password.";
        if (reason == "account_not_verified" || reason == "account_not_activated")
            return "Verify your Archive.org email address in a browser, then sign in again.";
        if (reason == "rate_limited" || reason == "too_many_requests")
            return "Archive.org has temporarily limited sign-in attempts. Wait before trying again.";
        if (reason == "captcha_required" || reason == "account_locked" || reason == "two_factor_required")
            return "Archive.org requires an account check. Sign in at archive.org in a browser first, then retry here.";
        return "Archive.org could not sign in. Check your account in a browser, then try again.";
    }
    const auto& cookies = response["values"]["cookies"];
    const std::string user = cookie_value(cookies["logged-in-user"].str()), sig = cookie_value(cookies["logged-in-sig"].str());
    std::lock_guard<std::mutex> lock(session_lock);
    if (!set_session(user, sig)) return "Archive.org returned an incomplete sign-in session.";
    if (!session_path.empty()) {
        Json saved = Json::object();
        saved.set("user", user);
        saved.set("sig", sig);
        if (!write_file_atomic(session_path, saved.dump())) LOG("archive sign-in: the session could not be saved for the next launch");
    }
    return "";
}
std::string archive_sign_in(const std::string& email, const std::string& password) {
    if (trim(email).empty() || password.empty()) return "Enter your Archive.org email and password.";
    HttpRequest request;
    request.url = "https://archive.org/services/xauthn/?op=login";
    request.method = "POST";
    request.body = "email=" + url_encode(trim(email)) + "&password=" + url_encode(password);
    request.headers.emplace_back("Content-Type", "application/x-www-form-urlencoded");
    request.follow_redirects = false;
    request.no_auto_auth = true;
    request.timeout_seconds = 20;
    request.max_bytes = 256 * 1024;
    LOG("archive sign-in: starting request (credentials omitted)");
    const auto response = http_request(request);
    bool ok = false;
    const auto parsed = Json::parse(response.body, &ok);
    LOG("archive sign-in: HTTP %d, JSON %s, transport error %s", response.status, ok ? "yes" : "no",
        response.error.empty() ? "no" : "yes");
    // Archive.org returns useful JSON error reasons with HTTP 401. Parse those
    // before the generic HTTP failure, without accepting a session on non-2xx.
    std::string problem;
    if (ok && parsed.has("success") && !parsed["success"].boolean()) problem = archive_accept_login(parsed);
    else if (!response.ok()) {
        if (response.status == 429) problem = "Archive.org has temporarily limited sign-in attempts. Wait before trying again.";
        else if (response.status == 401) problem = "Archive.org rejected this sign-in (HTTP 401). Check the email and password.";
        else if (response.status == 403) problem = "Archive.org blocked the sign-in request (HTTP 403). Check your account in a browser.";
        else if (response.status >= 300 && response.status < 400) problem = "Archive.org redirected sign-in unexpectedly. Try signing in on archive.org in a browser.";
        else problem = "Archive.org sign-in: " + describe_failure(response);
    } else if (!ok || !parsed.has("success")) problem = "Archive.org returned an unreadable sign-in response.";
    else problem = archive_accept_login(parsed);
    // Only our fixed diagnostic text is recorded, never response bodies or credentials.
    LOG("archive sign-in: %s", problem.empty() ? "session accepted" : problem.c_str());
    return problem;
}
}
