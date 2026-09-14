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
