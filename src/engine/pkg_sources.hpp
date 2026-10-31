// SPDX-License-Identifier: GPL-3.0-or-later
// Packages the engine serves to the console's installer over HTTP:
// sceAppInstUtilInstallByPackage takes an http:// URL as well as a path
// (ps5-web-file-manager src/pkg_installer.c does this for SMB packages), so a
// package the installer cannot see itself (on a network server, a paired
// engine) is handed over as http://127.0.0.1:<port>/pkg-source/<token>.pkg.
//
// Every request the installer makes is recorded (/api/pkg-sources): how it
// reads a package (order, range sizes, how often) is what streaming a
// package built on the fly would have to serve.
#pragma once

#include "alembic/result.hpp"
#include "http.hpp"
#include "json_value.hpp"

#include <string>

namespace alembic::engine::pkg_sources {

// Opens `path` (any path the engine reads, mounted ones included) and
// returns the token its URL carries. Sources stay until the engine stops:
// the installer keeps reading after it accepted the package.
Result<std::string> add(const std::string& path);

// GET or HEAD /pkg-source/<token>.pkg, with Range. Only from this console
// (127.0.0.1): the token is the only other guard.
HttpResponse serve(const HttpRequest& request);

// Every source with the requests it got: for /api/pkg-sources.
JsonValue report();

} // namespace alembic::engine::pkg_sources
