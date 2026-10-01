# Vendored third-party headers

These headers are checked in so the C++ port builds with nothing but a C++17
compiler — no package manager, no network access at build time.

| Path | Library | Version | License | Source |
| --- | --- | --- | --- | --- |
| `nlohmann/json.hpp` | [JSON for Modern C++](https://github.com/nlohmann/json) | v3.11.3 (single header) | MIT | `https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp` |
| `argparse/argparse.hpp` | [p-ranav/argparse](https://github.com/p-ranav/argparse) | v3.1 (single header) | MIT | `https://raw.githubusercontent.com/p-ranav/argparse/v3.1/include/argparse/argparse.hpp` |

Both libraries are MIT licensed: "Permission is hereby granted, free of charge,
to any person obtaining a copy of this software and associated documentation
files..." — see each upstream repository for the full license text.

JSON handling uses `nlohmann::ordered_json` so object key order is preserved
(matching the Python `dict` behaviour of the original tool). HTTP uses the
WinHTTP API that ships with Windows, so no TLS library is vendored.
