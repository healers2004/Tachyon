#include "commands/store.h"

#include <gtest/gtest.h>

#include <string>

using tachyon::Store;

TEST(Store, GetMissingKeyIsNullopt) {
    Store store;
    EXPECT_FALSE(store.get("missing").has_value());
}

TEST(Store, SetThenGet) {
    Store store;
    store.set("foo", "bar");
    ASSERT_TRUE(store.get("foo").has_value());
    EXPECT_EQ(*store.get("foo"), "bar");  // * reads the value out of the optional
}

TEST(Store, SetOverwrites) {
    Store store;
    store.set("foo", "bar");
    store.set("foo", "baz");
    EXPECT_EQ(*store.get("foo"), "baz");
}

TEST(Store, ValuesAreBinarySafe) {
    Store store;
    std::string value("a\0b\r\nc", 6);  // the length argument keeps the \0 inside the string
    store.set("k", value);
    EXPECT_EQ(*store.get("k"), value);
}

TEST(Store, DelReportsWhetherKeyExisted) {
    Store store;
    store.set("foo", "bar");
    EXPECT_TRUE(store.del("foo"));
    EXPECT_FALSE(store.del("foo"));
    EXPECT_FALSE(store.exists("foo"));
}

TEST(Store, EmptyKeyAndValueAreValid) {
    Store store;
    store.set("", "");
    EXPECT_TRUE(store.exists(""));
    EXPECT_EQ(*store.get(""), "");
}
