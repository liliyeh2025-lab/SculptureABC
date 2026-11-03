// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The update check's feed and version comparison (src/core/UpdateCheck.cpp).
#include "TestHarness.h"

#include "core/UpdateCheck.h"

#include <fstream>
#include <iterator>

using namespace sf;

TEST(update_versions_compare_numerically) {
    CHECK(compareVersions("0.6.0", "0.5.0") > 0);
    CHECK(compareVersions("0.5.0", "0.6.0") < 0);
    CHECK(compareVersions("0.10.0", "0.9.2") > 0); // not as text
    CHECK(compareVersions("0.6", "0.6.0") == 0);
    CHECK(compareVersions("v0.6.1", "0.6.0") > 0);
    CHECK(compareVersions("0.6.0-rc1", "0.6.0") == 0);
    CHECK(compareVersions("1.0.0", "0.99.99") > 0);
    CHECK(compareVersions("", "0.1.0") < 0);
}

TEST(update_feed_parses) {
    UpdateInfo u;
    std::string err;
    CHECK(parseUpdateFeed(R"({"version":"0.6.0","download":"https://example.org/dl","notes":"https://example.org/new",
                              "highlights":["Faster","Lighter","",3,"More","Even more","Too many"]})",
                          u, err));
    CHECK(u.version == "0.6.0" && u.downloadUrl == "https://example.org/dl" && u.notesUrl == "https://example.org/new");
    CHECK(u.highlights.size() == 4 && u.highlights[0] == "Faster" && u.highlights[2] == "More");
    // Notes fall back to the download page.
    CHECK(parseUpdateFeed(R"({"version":"0.6.0","download":"https://example.org/dl"})", u, err) &&
          u.notesUrl == "https://example.org/dl" && u.highlights.empty());
}

TEST(update_feed_rejects_bad_feeds) {
    UpdateInfo u;
    std::string err;
    CHECK(!parseUpdateFeed("not json", u, err));
    CHECK(!parseUpdateFeed("[]", u, err));
    CHECK(!parseUpdateFeed(R"({"download":"https://example.org/dl"})", u, err));
    CHECK(!parseUpdateFeed(R"({"version":"soon","download":"https://example.org/dl"})", u, err));
    CHECK(!parseUpdateFeed(R"({"version":"0.6.0","download":"http://example.org/dl"})", u, err)); // https only
    CHECK(!parseUpdateFeed(R"x({"version":"0.6.0","download":"javascript:alert(1)"})x", u, err));
    CHECK(!err.empty());
}

TEST(update_check_once_a_day) {
    CHECK(updateCheckDue(0, 1000));
    CHECK(!updateCheckDue(1000, 1000 + kUpdateCheckInterval - 1));
    CHECK(updateCheckDue(1000, 1000 + kUpdateCheckInterval));
    CHECK(updateCheckDue(5000, 1000)); // the clock went back
}

TEST(update_feed_on_the_website_parses) {
    std::ifstream f(SF_TEST_DATA_DIR "/../../website/latest.json", std::ios::binary);
    CHECK(bool(f));
    UpdateInfo u;
    std::string err;
    CHECK(parseUpdateFeed(std::string(std::istreambuf_iterator<char>(f), {}), u, err));
    CHECK(!u.highlights.empty());
}
