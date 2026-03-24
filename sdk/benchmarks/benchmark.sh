#!/bin/sh
# Silex benchmark: package install + compile minimal usage for 6 projects.
# Compares silex:slim (clang + apk) vs ubuntu:24.04 (gcc + apt).
# Usage: sh benchmarks/benchmark.sh
#
# Requires: docker, silex:slim image already built (make build or make bootstrap).
# Takes ~30 minutes (4 runs per project, 12 measurements total).

set -e

RUNS=4   # 4 runs per image per project; drop highest, average remaining 3

# Run command in container, print elapsed ms to stdout.
_time_run() {
    _img="$1"
    _cmd="$2"
    _t0=$(date +%s%3N)
    docker run --rm "$_img" sh -c "$_cmd" >/dev/null 2>&1
    _t1=$(date +%s%3N)
    printf '%d' $((_t1 - _t0))
}

# bench LABEL IMAGE CMD — prints avg ms (drop-highest of RUNS runs).
bench() {
    _label="$1"
    _image="$2"
    _cmd="$3"
    _results=""
    _i=0
    while [ "$_i" -lt "$RUNS" ]; do
        _dt=$(_time_run "$_image" "$_cmd")
        _results="$_results $_dt"
        printf '  %s r%d: %dms\n' "$_label" "$((_i+1))" "$_dt" >&2
        _i=$((_i + 1))
    done
    _sorted=$(printf '%s\n' $_results | sort -n)
    _a=$(printf '%s\n' $_sorted | sed -n '1p')
    _b=$(printf '%s\n' $_sorted | sed -n '2p')
    _c=$(printf '%s\n' $_sorted | sed -n '3p')
    printf '%d' $(((_a + _b + _c) / 3))
}

# Format ms with thousands separator.
comma() {
    _n=$1
    if [ "$_n" -ge 10000 ]; then
        printf '%d,%03d' $((_n / 1000)) $((_n % 1000))
    else
        printf '%d' "$_n"
    fi
}

# Print speedup as N.Nx.
speedup() {
    awk "BEGIN{printf \"%.1fx\",($2+0)/($1+0)}"
}

printf "Silex benchmark: package install + compile one file\n"
printf "silex:slim (clang+apk) vs ubuntu:24.04 (gcc+apt)\n"
printf "Runs per project: %d (drop highest, average rest)\n\n" "$RUNS"

docker pull ubuntu:24.04 -q >/dev/null 2>&1

# ============================================================================
# nlohmann/json (header-only)
# ============================================================================
printf "\n=== nlohmann/json ===\n" >&2
S_NLOHMANN=$(bench silex silex:slim \
    'apk add -q nlohmann-json-dev && printf "#include<nlohmann/json.hpp>\nint main(){auto j=nlohmann::json::parse(\"{\\\"x\\\":1}\");return 0;}" > /t.cpp && clang++ -std=c++17 -O2 /t.cpp -o /t')
U_NLOHMANN=$(bench ubuntu ubuntu:24.04 \
    'DEBIAN_FRONTEND=noninteractive apt-get update -qq 2>/dev/null && apt-get install -y -qq --no-install-recommends g++ nlohmann-json3-dev 2>/dev/null && printf "#include<nlohmann/json.hpp>\nint main(){return 0;}" > /t.cpp && g++ -std=c++17 -O2 /t.cpp -o /t')

# ============================================================================
# fmtlib
# ============================================================================
printf "\n=== fmtlib ===\n" >&2
S_FMT=$(bench silex silex:slim \
    'apk add -q fmt-dev && printf "#include<fmt/core.h>\nint main(){fmt::print(\"{}\",1);return 0;}" > /t.cpp && clang++ -std=c++17 -O2 /t.cpp -lfmt -o /t')
U_FMT=$(bench ubuntu ubuntu:24.04 \
    'DEBIAN_FRONTEND=noninteractive apt-get update -qq 2>/dev/null && apt-get install -y -qq --no-install-recommends g++ libfmt-dev 2>/dev/null && printf "#include<fmt/core.h>\nint main(){fmt::print(\"{}\",1);return 0;}" > /t.cpp && g++ -std=c++17 -O2 /t.cpp -lfmt -o /t')

# ============================================================================
# googletest
# ============================================================================
printf "\n=== googletest ===\n" >&2
S_GTEST=$(bench silex silex:slim \
    'apk add -q gtest-dev && printf "#include<gtest/gtest.h>\nTEST(X,Y){EXPECT_EQ(1,1);}\nint main(int c,char**v){::testing::InitGoogleTest(&c,v);return RUN_ALL_TESTS();}" > /t.cpp && clang++ -std=c++17 -O2 /t.cpp -lgtest -lgtest_main -pthread -o /t')
U_GTEST=$(bench ubuntu ubuntu:24.04 \
    'DEBIAN_FRONTEND=noninteractive apt-get update -qq 2>/dev/null && apt-get install -y -qq --no-install-recommends g++ libgtest-dev 2>/dev/null && printf "#include<gtest/gtest.h>\nTEST(X,Y){EXPECT_EQ(1,1);}\nint main(int c,char**v){::testing::InitGoogleTest(&c,v);return RUN_ALL_TESTS();}" > /t.cpp && g++ -std=c++17 -O2 /t.cpp -lgtest -lgtest_main -pthread -o /t')

# ============================================================================
# abseil-cpp
# ============================================================================
printf "\n=== abseil-cpp ===\n" >&2
S_ABSL=$(bench silex silex:slim \
    'apk add -q abseil-cpp-20250127-dev && printf "#include<absl/strings/str_join.h>\nint main(){std::vector<std::string> v={\"a\",\"b\"};absl::StrJoin(v,\",\");return 0;}" > /t.cpp && clang++ -std=c++17 -O2 /t.cpp -labsl_strings -o /t')
U_ABSL=$(bench ubuntu ubuntu:24.04 \
    'DEBIAN_FRONTEND=noninteractive apt-get update -qq 2>/dev/null && apt-get install -y -qq --no-install-recommends g++ libabsl-dev 2>/dev/null && printf "#include<absl/strings/str_join.h>\nint main(){std::vector<std::string> v={\"a\",\"b\"};absl::StrJoin(v,\",\");return 0;}" > /t.cpp && g++ -std=c++17 -O2 /t.cpp -labsl_strings -o /t')

# ============================================================================
# re2
# ============================================================================
printf "\n=== re2 ===\n" >&2
S_RE2=$(bench silex silex:slim \
    'apk add -q re2-dev && printf "#include<re2/re2.h>\nint main(){re2::RE2 r(\"a+\");return re2::RE2::FullMatch(\"aaa\",r)?0:1;}" > /t.cpp && clang++ -std=c++17 -O2 /t.cpp -lre2 -o /t')
U_RE2=$(bench ubuntu ubuntu:24.04 \
    'DEBIAN_FRONTEND=noninteractive apt-get update -qq 2>/dev/null && apt-get install -y -qq --no-install-recommends g++ libre2-dev 2>/dev/null && printf "#include<re2/re2.h>\nint main(){re2::RE2 r(\"a+\");return re2::RE2::FullMatch(\"aaa\",r)?0:1;}" > /t.cpp && g++ -std=c++17 -O2 /t.cpp -lre2 -o /t')

# ============================================================================
# SQLite amalgam (compile from source; clang -O3 vs gcc -O3)
# ============================================================================
printf "\n=== SQLite amalgam ===\n" >&2
S_SQLITE=$(bench silex silex:slim \
    'apk add -q curl unzip && curl -fsSL https://www.sqlite.org/2024/sqlite-amalgamation-3470200.zip -o /s.zip && unzip -q /s.zip -d /s && clang -O3 /s/sqlite-amalgamation-3470200/sqlite3.c -o /sqlite3 -lpthread -ldl -lm')
U_SQLITE=$(bench ubuntu ubuntu:24.04 \
    'DEBIAN_FRONTEND=noninteractive apt-get update -qq 2>/dev/null && apt-get install -y -qq --no-install-recommends gcc curl unzip 2>/dev/null && curl -fsSL https://www.sqlite.org/2024/sqlite-amalgamation-3470200.zip -o /s.zip && unzip -q /s.zip -d /s && gcc -O3 /s/sqlite-amalgamation-3470200/sqlite3.c -o /sqlite3 -lpthread -ldl -lm')

# ============================================================================
# Results table
# ============================================================================
printf "\nResults (4 runs, drop highest, 3-run average):\n"
printf "%-18s  %9s  %9s  %s\n" "project" "silex" "ubuntu" "speedup"
for _row in \
    "nlohmann/json:$S_NLOHMANN:$U_NLOHMANN" \
    "fmtlib:$S_FMT:$U_FMT" \
    "googletest:$S_GTEST:$U_GTEST" \
    "abseil-cpp:$S_ABSL:$U_ABSL" \
    "google/re2:$S_RE2:$U_RE2" \
    "SQLite amalgam:$S_SQLITE:$U_SQLITE"; do
    _lbl="${_row%%:*}"
    _rest="${_row#*:}"
    _s="${_rest%%:*}"
    _u="${_rest#*:}"
    printf "%-18s  %6sms   %6sms   %s\n" "$_lbl" "$(comma "$_s")" "$(comma "$_u")" "$(speedup "$_s" "$_u")"
done

printf "\nSystem: %s\n" "$(uname -srm)"
printf "Docker: %s\n" "$(docker --version)"
