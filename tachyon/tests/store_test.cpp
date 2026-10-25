#include "commands/store.h"

#include <gtest/gtest.h>

#include <string>

using tachyon::Hash;
using tachyon::kNoExpiry;
using tachyon::List;
using tachyon::Store;
using tachyon::Value;

namespace {

const std::int64_t kNow = 1'000'000;  // ' separates digits for readability

}  // namespace

TEST(Store, FindMissingKeyIsNull) {
    Store store;
    EXPECT_EQ(store.find("missing", kNow), nullptr);
}

TEST(Store, SetThenFind) {
    Store store;
    store.set("foo", std::string("bar"));
    Value* value = store.find("foo", kNow);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(std::get<std::string>(*value), "bar");
}

TEST(Store, SetReplacesAnyType) {
    Store store;
    store.set("k", List{"a", "b"});
    store.set("k", std::string("now a string"));
    EXPECT_TRUE(std::holds_alternative<std::string>(*store.find("k", kNow)));
}

TEST(Store, ValuesAreBinarySafe) {
    Store store;
    std::string value("a\0b\r\nc", 6);  // the length argument keeps the \0 inside the string
    store.set("k", value);
    EXPECT_EQ(std::get<std::string>(*store.find("k", kNow)), value);
}

TEST(Store, DelReportsWhetherKeyExisted) {
    Store store;
    store.set("foo", std::string("bar"));
    EXPECT_TRUE(store.del("foo"));
    EXPECT_FALSE(store.del("foo"));
    EXPECT_EQ(store.find("foo", kNow), nullptr);
}

TEST(Store, EmptyKeyAndValueAreValid) {
    Store store;
    store.set("", std::string(""));
    EXPECT_NE(store.find("", kNow), nullptr);
}

TEST(Store, ExpiredKeyIsDeletedOnAccess) {
    Store store;
    store.set("k", std::string("v"));
    store.set_expiry("k", kNow + 100);
    EXPECT_NE(store.find("k", kNow + 99), nullptr);   // one millisecond before the deadline
    EXPECT_EQ(store.find("k", kNow + 100), nullptr);  // at the deadline it's gone
    EXPECT_EQ(store.size(), 0u);                      // and actually removed
    EXPECT_EQ(store.expiry("k"), kNoExpiry);
}

TEST(Store, SetClearsExpiryUnlessKept) {
    Store store;
    store.set("k", std::string("v"));
    store.set_expiry("k", kNow + 100);
    store.set("k", std::string("w"), true);
    EXPECT_EQ(store.expiry("k"), kNow + 100);
    store.set("k", std::string("x"));
    EXPECT_EQ(store.expiry("k"), kNoExpiry);
}

TEST(Store, SweepRemovesOnlyExpiredKeys) {
    Store store;
    store.set("old", std::string("1"));
    store.set_expiry("old", kNow - 1);
    store.set("later", std::string("2"));
    store.set_expiry("later", kNow + 1000);
    store.set("forever", Hash{{"f", "v"}});
    EXPECT_EQ(store.remove_expired(kNow), 1u);
    EXPECT_EQ(store.size(), 2u);  // removed without anyone reading it
    EXPECT_EQ(store.expiry("later"), kNow + 1000);
}

TEST(Store, ClearRemovesKeysAndExpiries) {
    Store store;
    store.set("k", std::string("v"));
    store.set_expiry("k", kNow + 100);
    store.clear();
    EXPECT_EQ(store.size(), 0u);
    EXPECT_EQ(store.expiry("k"), kNoExpiry);
}
