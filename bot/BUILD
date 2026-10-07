load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# The bot itself: what joins Discord to the market, and to what gm_bot knows.
package(default_visibility = [
    "//:__pkg__",
    "//perf:__pkg__",
])

cc_library(
    name = "bot",
    srcs = [
        "arguments.cc",
        "bot.cc",
        "config.cc",
        "report.cc",
    ],
    hdrs = [
        "arguments.h",
        "bot.h",
        "config.h",
        "report.h",
    ],
    deps = [
        "//gm",
        "//market",
        "@abseil-cpp",
        "@bedrock//:async",
        "@bedrock//:discord",
        "@bedrock//:net",
    ],
)

[
    cc_test(
        name = name,
        size = "small",
        srcs = [name + ".cc"],
        deps = [
            ":bot",
            "//gm",
            "//gm:fake_ledger",
            "//market",
            "@abseil-cpp",
            "@bedrock//:async",
            "@bedrock//:discord",
            "@bedrock//:http",
            "@bedrock//:json",
            "@bedrock//:net",
            "@bedrock//:testing_main",
            "@bedrock//:websocket",
            "@gbenchmark",
            "@gtest",
        ],
    )
    for name in [
        "arguments_test",
        "bot_test",
        "config_test",
        "report_test",
    ]
]

# What people type into commands, whatever they type.
cc_test(
    name = "arguments_fuzz_test",
    size = "small",
    srcs = ["arguments_fuzz_test.cc"],
    deps = [
        ":bot",
        "//market",
        "@abseil-cpp",
        "@bedrock//:discord",
        "@fuzztest",
        "@fuzztest//:main",
        "@gtest",
    ],
)
