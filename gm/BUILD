load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# What gm_bot knows, as far as this bot needs it: which messages are a GM,
# and who has said one. Read from gm_bot's ledger; never written.
package(default_visibility = ["//:__subpackages__"])

cc_library(
    name = "gm",
    srcs = [
        "phrase.cc",
        "record.cc",
    ],
    hdrs = [
        "phrase.h",
        "record.h",
    ],
    deps = [
        "@abseil-cpp",
        "@bedrock//:sqlite",
    ],
)

# A ledger laid out as gm_bot's is, for tests to fill in.
cc_library(
    name = "fake_ledger",
    testonly = True,
    srcs = ["fake_ledger.cc"],
    hdrs = ["fake_ledger.h"],
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
            ":fake_ledger",
            ":gm",
            "@abseil-cpp",
            "@bedrock//:sqlite",
            "@bedrock//:testing_main",
            "@gbenchmark",
            "@gtest",
        ],
    )
    for name in [
        "fake_ledger_test",
        "phrase_test",
        "record_test",
    ]
]

# What counts as a GM against a model of gm_bot's rule, on arbitrary
# messages.
cc_test(
    name = "phrase_fuzz_test",
    size = "small",
    srcs = ["phrase_fuzz_test.cc"],
    deps = [
        ":gm",
        "@abseil-cpp",
        "@fuzztest",
        "@fuzztest//:main",
        "@gtest",
    ],
)
