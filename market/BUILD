load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# The prediction market: gmbux, prices, and the exchange that keeps who
# holds what. Knows nothing of Discord, nor of how a GM is recognised.
package(default_visibility = ["//:__subpackages__"])

cc_library(
    name = "market",
    srcs = [
        "exchange.cc",
        "gmbux.cc",
        "lmsr.cc",
        "time_of_day.cc",
    ],
    hdrs = [
        "exchange.h",
        "gmbux.h",
        "lmsr.h",
        "time_of_day.h",
    ],
    deps = [
        "@abseil-cpp",
        "@bedrock//:sqlite",
    ],
)

[
    cc_test(
        name = name,
        size = "small",
        srcs = [name + ".cc"],
        deps = [
            ":market",
            "@abseil-cpp",
            "@bedrock//:sqlite",
            "@bedrock//:testing_main",
            "@gbenchmark",
            "@gtest",
        ],
    )
    for name in [
        "exchange_test",
        "lmsr_test",
        "gmbux_test",
        "time_of_day_test",
    ]
]

# The exchange against a model, and the properties its prices and times of
# day must have, on arbitrary inputs. FuzzTest brings its own main, which is
# what understands --fuzz.
[
    cc_test(
        name = name,
        size = "small",
        srcs = [name + ".cc"],
        deps = [
            ":market",
            "@abseil-cpp",
            "@fuzztest",
            "@fuzztest//:main",
            "@gtest",
        ],
    )
    for name in [
        "exchange_fuzz_test",
        "lmsr_fuzz_test",
        "time_of_day_fuzz_test",
    ]
]
