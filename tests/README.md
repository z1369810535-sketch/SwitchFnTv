# Feiniu catalog regression checks

These tests compile the production `app/src/api/fntv.cpp` with fake HTTP responses
and minimal UI/configuration stubs. No NAS login, network request, or user data is
needed. They cover response formats, library queries, pagination, season typing,
episode ordering, and aggregation across seasons. They do not verify rendering,
Switch linking, or playback on hardware.

Run from the repository root with a C++17 compiler:

```sh
mkdir -p build-tests
c++ -std=c++17 -pthread -Itests/stubs -Iapp/include \
  -Ilibrary/borealis/library/include/borealis/extern \
  tests/fntv_catalog_test.cpp app/src/api/fntv.cpp -o build-tests/fntv_catalog_test
./build-tests/fntv_catalog_test
```

Other checks, same compiler flags:

- `tests/fntv_playback_test.cpp` — resume positions, local history, favorites.
- `tests/fntv_compat_test.cpp` — error messages, retry policy (a rejected password is
  never re-sent, an offline NAS is not retried for a minute), sort columns with fallback,
  watched marks, episode titles, fallback stream URL.

```sh
c++ -std=c++17 -pthread -Itests/stubs -Iapp/include \
  -Ilibrary/borealis/library/include/borealis/extern \
  tests/fntv_compat_test.cpp app/src/api/fntv.cpp app/src/utils/playback_history.cpp \
  -o build-tests/fntv_compat_test
./build-tests/fntv_compat_test
```

`python3 tests/ui_resources_test.py` checks that every layout parses, every `BRLS_BIND`
id exists in its layout, and every translation key used in code or layouts exists in
Simplified Chinese and English.
