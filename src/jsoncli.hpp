// Non-interactive JSON mode (--json / --request / --request-file).
//
// One JSON request document goes in, exactly one JSON response document comes
// out, and no interactive prompt is ever shown.  Everything the interactive
// code path would print on stdout (progress lines, verbose logs, cache
// messages) is redirected to stderr for the duration of the request so stdout
// stays machine readable.
#pragma once

#include <string>

namespace tuf {

struct JsonCliOptions {
    // Inline request document.  Empty (or "-") means "read stdin".
    std::string request;
    // Request file.  Only consulted when `request` is empty.
    std::string request_file;
    // Pretty print with `indent` spaces instead of one compact line.
    bool pretty = false;
    int indent = 2;
};

// Runs the request and prints one JSON document to stdout.
// Returns 0 when every request succeeded, 1 when at least one operation
// failed, and 2 when the request document itself could not be handled.
int run_json_cli(const JsonCliOptions& options);

}  // namespace tuf
