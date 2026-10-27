#include "server/pubsub.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "protocol/resp_writer.h"
#include "server/connection.h"

using tachyon::Connection;
using tachyon::PubSub;
using tachyon::Socket;
using tachyon::Store;

namespace {

// Sends one command the way a client would, and returns (and clears) the reply bytes queued
// for that connection.
std::string send(Connection& connection, Store& store, PubSub& pubsub,
                 const std::vector<std::string>& args) {
    tachyon::handle_input(connection, store, nullptr, pubsub, tachyon::encode_command(args));
    std::string reply = connection.pending_reply;
    connection.pending_reply.clear();
    return reply;
}

std::string subscribed(const std::string& channel, int count) {
    return "*3\r\n$9\r\nsubscribe\r\n$" + std::to_string(channel.size()) + "\r\n" + channel +
           "\r\n:" + std::to_string(count) + "\r\n";
}

std::string unsubscribed(const std::string& channel, int count) {
    return "*3\r\n$11\r\nunsubscribe\r\n$" + std::to_string(channel.size()) + "\r\n" + channel +
           "\r\n:" + std::to_string(count) + "\r\n";
}

std::string message(const std::string& channel, const std::string& text) {
    return "*3\r\n$7\r\nmessage\r\n$" + std::to_string(channel.size()) + "\r\n" + channel +
           "\r\n$" + std::to_string(text.size()) + "\r\n" + text + "\r\n";
}

const std::string kRefusal =
    "': only (P|S)SUBSCRIBE / (P|S)UNSUBSCRIBE / PING / QUIT / RESET are allowed in this "
    "context\r\n";

}  // namespace

TEST(PubSub, SubscribeConfirmsEachChannel) {
    Store store;
    PubSub pubsub;  // declared before every connection, so it outlives them
    Connection client{Socket()};
    EXPECT_EQ(send(client, store, pubsub, {"SUBSCRIBE", "a", "b"}),
              subscribed("a", 1) + subscribed("b", 2));
    // Subscribing again is confirmed but doesn't count twice.
    EXPECT_EQ(send(client, store, pubsub, {"subscribe", "a"}), subscribed("a", 2));
    EXPECT_EQ(send(client, store, pubsub, {"SUBSCRIBE"}),
              "-ERR wrong number of arguments for 'subscribe' command\r\n");
}

TEST(PubSub, PublishReachesEverySubscriber) {
    Store store;
    PubSub pubsub;
    Connection publisher{Socket()};
    std::vector<std::unique_ptr<Connection>> subscribers;
    for (int i = 0; i < 50; ++i) {
        subscribers.push_back(std::make_unique<Connection>(Socket()));
        send(*subscribers.back(), store, pubsub, {"SUBSCRIBE", "news"});
    }
    Connection other{Socket()};
    send(other, store, pubsub, {"SUBSCRIBE", "sports"});

    EXPECT_EQ(send(publisher, store, pubsub, {"PUBLISH", "news", "hello"}), ":50\r\n");
    for (const std::unique_ptr<Connection>& subscriber : subscribers) {
        EXPECT_EQ(subscriber->pending_reply, message("news", "hello"));
    }
    EXPECT_EQ(other.pending_reply, "");  // a different channel hears nothing
}

TEST(PubSub, PublishToNobody) {
    Store store;
    PubSub pubsub;
    Connection publisher{Socket()};
    EXPECT_EQ(send(publisher, store, pubsub, {"PUBLISH", "empty", "x"}), ":0\r\n");
    EXPECT_EQ(send(publisher, store, pubsub, {"PUBLISH", "a"}),
              "-ERR wrong number of arguments for 'publish' command\r\n");
}

// Messages are delivered as given: any bytes, including line endings, and the empty message.
TEST(PubSub, MessagesAreBinarySafe) {
    Store store;
    PubSub pubsub;
    Connection subscriber{Socket()};
    Connection publisher{Socket()};
    send(subscriber, store, pubsub, {"SUBSCRIBE", "c"});
    send(publisher, store, pubsub, {"PUBLISH", "c", "a\r\nb"});
    send(publisher, store, pubsub, {"PUBLISH", "c", ""});
    EXPECT_EQ(subscriber.pending_reply, message("c", "a\r\nb") + message("c", ""));
}

TEST(PubSub, UnsubscribeNamedAndAll) {
    Store store;
    PubSub pubsub;
    Connection client{Socket()};
    send(client, store, pubsub, {"SUBSCRIBE", "a", "b", "c"});
    EXPECT_EQ(send(client, store, pubsub, {"UNSUBSCRIBE", "a", "zz"}),
              unsubscribed("a", 2) + unsubscribed("zz", 2));

    // No names: leaves every channel. The order is the set's, so compare the counts.
    std::string reply = send(client, store, pubsub, {"UNSUBSCRIBE"});
    EXPECT_TRUE(reply == unsubscribed("b", 1) + unsubscribed("c", 0) ||
                reply == unsubscribed("c", 1) + unsubscribed("b", 0))
        << reply;
    EXPECT_EQ(pubsub.channel_count(), 0u);  // empty channels are removed

    // Nothing left: Redis replies with a nil channel name.
    EXPECT_EQ(send(client, store, pubsub, {"UNSUBSCRIBE"}),
              "*3\r\n$11\r\nunsubscribe\r\n$-1\r\n:0\r\n");
}

TEST(PubSub, SubscribedModeRefusesOtherCommands) {
    Store store;
    PubSub pubsub;
    Connection client{Socket()};
    send(client, store, pubsub, {"SUBSCRIBE", "a"});
    EXPECT_EQ(send(client, store, pubsub, {"GET", "k"}), "-ERR Can't execute 'get" + kRefusal);
    EXPECT_EQ(send(client, store, pubsub, {"PUBLISH", "a", "m"}),
              "-ERR Can't execute 'publish" + kRefusal);
    // Errors that Redis checks first still come first.
    EXPECT_EQ(send(client, store, pubsub, {"GET"}),
              "-ERR wrong number of arguments for 'get' command\r\n");
    EXPECT_EQ(send(client, store, pubsub, {"FLY"}),
              "-ERR unknown command 'FLY', with args beginning with: \r\n");
    // PING still works, as an array.
    EXPECT_EQ(send(client, store, pubsub, {"PING"}), "*2\r\n$4\r\npong\r\n$0\r\n\r\n");
    EXPECT_EQ(send(client, store, pubsub, {"PING", "x"}), "*2\r\n$4\r\npong\r\n$1\r\nx\r\n");
}

TEST(PubSub, LeavingTheLastChannelEndsSubscribedMode) {
    Store store;
    PubSub pubsub;
    Connection client{Socket()};
    send(client, store, pubsub, {"SUBSCRIBE", "a"});
    send(client, store, pubsub, {"UNSUBSCRIBE", "a"});
    EXPECT_EQ(send(client, store, pubsub, {"PING"}), "+PONG\r\n");
    EXPECT_EQ(send(client, store, pubsub, {"SET", "k", "v"}), "+OK\r\n");
}

// The dangling-pointer case: a subscriber is deleted (it disconnected) while the channel still
// exists. With the sanitizers on, any pointer left behind would crash the PUBLISH below.
TEST(PubSub, DisconnectedSubscriberIsForgotten) {
    Store store;
    PubSub pubsub;
    Connection stays{Socket()};
    Connection publisher{Socket()};
    send(stays, store, pubsub, {"SUBSCRIBE", "news"});
    {
        Connection leaves{Socket()};
        send(leaves, store, pubsub, {"SUBSCRIBE", "news", "only-me"});
        EXPECT_EQ(pubsub.channel_count(), 2u);
    }  // `leaves` is destroyed here, as the event loop does when a client hangs up
    EXPECT_EQ(pubsub.channel_count(), 1u);  // "only-me" had no one else and is gone
    EXPECT_EQ(send(publisher, store, pubsub, {"PUBLISH", "news", "hi"}), ":1\r\n");
    EXPECT_EQ(send(publisher, store, pubsub, {"PUBLISH", "only-me", "hi"}), ":0\r\n");
    EXPECT_EQ(stays.pending_reply, message("news", "hi"));
}

TEST(PubSub, ManySubscribersComeAndGo) {
    Store store;
    PubSub pubsub;
    Connection publisher{Socket()};
    std::vector<std::unique_ptr<Connection>> subscribers;
    for (int i = 0; i < 100; ++i) {
        subscribers.push_back(std::make_unique<Connection>(Socket()));
        send(*subscribers.back(), store, pubsub, {"SUBSCRIBE", "c", "d"});
    }
    // Delete every other one, from the middle of the list, as hang-ups would.
    for (std::size_t i = 0; i < subscribers.size(); i += 2) subscribers[i].reset();
    EXPECT_EQ(send(publisher, store, pubsub, {"PUBLISH", "c", "x"}), ":50\r\n");
    subscribers.clear();
    EXPECT_EQ(pubsub.channel_count(), 0u);
    EXPECT_EQ(send(publisher, store, pubsub, {"PUBLISH", "d", "x"}), ":0\r\n");
}

TEST(PubSub, QuitRepliesOkAndIgnoresTheRest) {
    Store store;
    PubSub pubsub;
    Connection client{Socket()};
    tachyon::handle_input(client, store, nullptr, pubsub,
                          tachyon::encode_command({"QUIT"}) + tachyon::encode_command({"PING"}));
    EXPECT_EQ(client.pending_reply, "+OK\r\n");
    EXPECT_TRUE(client.closing);
}

TEST(PubSub, OnlySubscribersHaveAnOutputLimit) {
    Store store;
    PubSub pubsub;
    Connection normal{Socket()};
    normal.pending_reply.assign(tachyon::kMaxSubscriberBacklogBytes + 1, 'x');
    EXPECT_FALSE(tachyon::over_output_limit(normal));

    Connection subscriber{Socket()};
    send(subscriber, store, pubsub, {"SUBSCRIBE", "c"});
    subscriber.pending_reply.assign(tachyon::kMaxSubscriberBacklogBytes, 'x');
    EXPECT_FALSE(tachyon::over_output_limit(subscriber));
    subscriber.pending_reply += 'x';
    EXPECT_TRUE(tachyon::over_output_limit(subscriber));
}
