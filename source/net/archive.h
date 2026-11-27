#pragma once
#include <string>
#include "core/json.h"

namespace lib {
// The password and S3 keys are never written to disk. Once
// archive_restore_session has named a file, the session cookies are kept there
// so the sign-in survives closing the app; signing out deletes it.
std::string archive_sign_in(const std::string& email, const std::string& password);
void archive_sign_out();
// At startup: where the session is kept, and loads one saved earlier.
// True when a saved session was restored. Calling it again reloads the file
// (the background downloader does when the app signs in or out).
bool archive_restore_session(const std::string& path);
bool archive_signed_in();
std::string archive_cookie_for(const std::string& url);
// Exposed for response validation tests. Empty result means success.
std::string archive_accept_login(const Json& response);
}
