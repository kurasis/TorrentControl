#include "runtime_policy.hpp"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Runtime API floor compares all numeric version components", "[runtime]")
{
    using tc::app::supports_webview_runtime;
    CHECK(supports_webview_runtime("113.0.1774.30"));
    CHECK(supports_webview_runtime("114.0.0.0"));
    CHECK(supports_webview_runtime("113.0.1774.31 beta"));
    CHECK(supports_webview_runtime("148.0.3682.44 dev"));
    CHECK_FALSE(supports_webview_runtime("99.0.9999.99"));
    CHECK_FALSE(supports_webview_runtime("113.0.1774.29"));
    CHECK_FALSE(supports_webview_runtime("113.0.1773.999"));
    for (auto bad : {"", "113", "113.0.1774", "113.0.1774.30.1", "113.0.1774.30 garbage",
                     "113.-1.1774.30", "4294967296.0.0.0", "113..1774.30"})
        CHECK_FALSE(supports_webview_runtime(bad));
    CHECK_FALSE(supports_webview_runtime("148.0.0.0", "invalid"));
    CHECK_FALSE(supports_webview_runtime("148.0.0.0", "999.0.0.0"));
}
