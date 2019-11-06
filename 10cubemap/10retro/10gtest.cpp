/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 *
 * Unit tests for libretro.
 */

#include "gtest/gtest.h"
#include "retroweb.h"

namespace {  // An anonymous namespace keeps any definition local to this file.

static const std::string goodURLhost = "http://buildbot.libretro.com";
static const std::string goodURLpath =
    "/nightly/linux/x86_64/latest/2048_libretro.so.zip";

// RetroHttpTest tests class RetroHttp in isolation.
class RetroHttpTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(http.getErrno(), -1);
    ASSERT_EQ(http.ctorError(), 0);
  }

  int numDataCalls{0};
  size_t totalLen{0};

 public:
  RetroHttp http;

  static int dataFn(void* self, void* data, size_t len, size_t maxlen) {
    return reinterpret_cast<RetroHttpTest*>(self)->onData(data, len, maxlen);
  }
  int onData(void* data, size_t len, size_t maxlen) {
    (void)data;
    (void)maxlen;
    numDataCalls++;
    totalLen += len;
    return 0;
  }

  int waitForData() {
    float progress = 2.f;
    while (http.pollGET(progress)) {
      if (progress < 0 || progress > 1) {
        return 1;
      }
    }
    return 0;
  }
};

TEST_F(RetroHttpTest, ctorOnly) {
  // ctorError is called by SetUp.
  ASSERT_EQ(http.getErrno(), 0);
}

TEST_F(RetroHttpTest, startGETnoDataCb) {
  ASSERT_EQ(http.getErrno(), 0);
  std::string url = goodURLhost + goodURLpath;
  ASSERT_EQ(http.startGET(url.c_str()), 1);
  ASSERT_EQ(http.getErrno(), 0);
  ASSERT_STREQ(http.getUrlPath(), "/") << "URL not set until after data cb";
}

TEST_F(RetroHttpTest, startGET) {
  ASSERT_EQ(http.getErrno(), 0);
  http.setDataCallback(this, dataFn);
  std::string url = goodURLhost + goodURLpath;
  ASSERT_EQ(http.startGET(url.c_str()), 0);
  ASSERT_EQ(http.getErrno(), 0);
  ASSERT_EQ(http.getUrlPath(), goodURLpath) << "no HTTP redirect expected";
  ASSERT_EQ(waitForData(), 0) << "Found progress fraction outside [0,1]";
  ASSERT_EQ(http.getErrno(), 0);
  EXPECT_EQ(http.getStatusCode(), 200);
  EXPECT_GE(numDataCalls, 1);
  EXPECT_GT(totalLen, 0u) << "Should not have a file of size 0";
}

class RetroWebTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(web.getLastSyncError(), -1);
    ASSERT_EQ(web.ctorError(), 0);
  }

 public:
  RetroWeb web;
};

TEST_F(RetroWebTest, ctorOnly) {
  // ctorError is called by SetUp.
  ASSERT_EQ(web.getLastSyncError(), 0);
}

TEST_F(RetroWebTest, startSync) {
  // ctorError is called by SetUp.
  ASSERT_EQ(web.getLastSyncError(), 0);
  ASSERT_EQ(web.startSync(), 0);
  while (web.isSyncRunning()) {
    ASSERT_EQ(web.poll(), 0);
    ASSERT_EQ(web.getLastSyncError(), 0);
  }
  ASSERT_EQ(web.getLastSyncError(), 0);
}

TEST_F(RetroWebTest, getAppLen) {
  // ctorError is called by SetUp.
  ASSERT_GT(web.getApps().size(), 0u);
}

TEST_F(RetroWebTest, generateCoresDb) {
  auto db = web.generateCoresDb();
  std::vector<std::string> keywords{
      "2048",
      "nestopia",
  };
  for (const std::string& kw : keywords) {
    if (db.find(kw) == std::string::npos) {
      ASSERT_TRUE(false) << "must contain " << kw << ": " << db;
    }
  }
}

}  // End of anonymous namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
