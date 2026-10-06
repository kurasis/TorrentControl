#pragma once

// Developer evidence only: the same AppService workflow in the console and
// real WebView2 host. No replacement engine or synthetic payload reader.
#include "tc/service/app_service.hpp"
#include "tc/core/torrent_engine.hpp"

namespace tc::proof {
inline nlohmann::json prepare_fixture(service::AppService& app, nlohmann::json const& input)
{
    app.new_draft();
    app.add_sources({core::path_from_utf8(input.at("root").get<std::string>())});
    app.wait_for_scan();
    app.update_draft({{"format", input.at("format")}, {"pieceLength", input.value("pieceLength", 262144)},
        {"trackers", nlohmann::json::array()}, {"creationDate", "omit"},
        {"creator", "TorrentControl workflow proof"}, {"replaceExisting", input.value("replace", false)},
        {"acceptLargeResourceUse", true}}, std::nullopt);
    app.set_output(core::path_from_utf8(input.at("output").get<std::string>()));
    auto const snapshot = app.snapshot();
    if (snapshot.at("scan").at("state") != "ready") throw std::runtime_error("Fixture scan failed");
    return {{"files", snapshot.at("scan").at("summary").at("realFiles")},
        {"engine", core::engine_version()}, {"format", input.at("format")}};
}
}
