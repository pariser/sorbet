load(":snapshot.bzl", _snapshot_test = "snapshot_test", _snapshot_tests = "snapshot_tests")

test_suite(
    name = "snapshot",
    tests = [
        _snapshot_test("partial/stupidedi")
    ],
)

sh_library(
    name = "logging",
    srcs = [ "logging.sh" ],
)
