#include "server/pubsub.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "protocol/resp_writer.h"
#include "server/connection.h"
#include "server/context.h"

using tachyon::Connection;
using tachyon::Socket;

namespace {

// Sends one command the way a client would, and returns (and clears) the reply bytes queued
// for that connection.
std::string send(Connection& connection, tachyon::ServerContext& context,
                 const std::vector<std::string>& args) {
    tachyon::handle_input(connection, context, tachyon::encode_command(args));
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
    tachyon::ServerContext context;  // declared before every connection, so it outlives them
    Connection client{Socket()};
    EXPECT_EQ(send(client, context, {"SUBSCRIBE", "a", "b"}),
              subscribed("a", 1) + subscribed("b", 2));
    // Subscribing again is confirmed but doesn't count twice.
    EXPECT_EQ(send(client, context, {"subscribe", "a"}), subscribed("a", 2));
    EXPECT_EQ(send(client, context, {"SUBSCRIBE"}),
              "-ERR wrong number of arguments for 'subscribe' command\r\n");
}

TEST(PubSub, PublishReachesEverySubscriber) {
    tachyon::ServerContext context;
    Connection publisher{Socket()};
    std::vector<std::unique_ptr<Connection>> subscribers;
    for (int i = 0; i < 50; ++i) {
        subscribers.push_back(std::make_unique<Connection>(Socket()));
        send(*subscribers.back(), context, {"SUBSCRIBE", "news"});
    }
    Connection other{Socket()};
    send(other, context, {"SUBSCRIBE", "sports"});

    EXPECT_EQ(send(publisher, context, {"PUBLISH", "news", "hello"}), ":50\r\n");
    for (const std::unique_ptr<Connection>& subscriber : subscribers) {
        EXPECT_EQ(subscriber->pending_reply, message("news", "hello"));
    }
    EXPECT_EQ(other.pending_reply, "");  // a different channel hears nothing
}

TEST(PubSub, PublishToNobody) {
    tachyon::ServerContext context;
    Connection publisher{Socket()};
    EXPECT_EQ(send(publisher, context, {"PUBLISH", "empty", "x"}), ":0\r\n");
    EXPECT_EQ(send(publisher, context, {"PUBLISH", "a"}),
              "-ERR wrong number of arguments for 'publish' command\r\n");
}

// Messages are delivered as given: any bytes, including line endings, and the empty message.
TEST(PubSub, MessagesAreBinarySafe) {
    tachyon::ServerContext context;
    Connection subscriber{Socket()};
    Connection publisher{Socket()};
    send(subscriber, context, {"SUBSCRIBE", "c"});
    send(publisher, context, {"PUBLISH", "c", "a\r\nb"});
    send(publisher, context, {"PUBLISH", "c", ""});
    EXPECT_EQ(subscriber.pending_reply, message("c", "a\r\nb") + message("c", ""));
}

TEST(PubSub, UnsubscribeNamedAndAll) {
    tachyon::ServerContext context;
    Connection client{Socket()};
    send(client, context, {"SUBSCRIBE", "a", "b", "c"});
    EXPECT_EQ(send(client, context, {"UNSUBSCRIBE", "a", "zz"}),
              unsubscribed("a", 2) + unsubscribed("zz", 2));

    // No names: leaves every channel. The order is the set's, so compare the counts.
    std::string reply = send(client, context, {"UNSUBSCRIBE"});
    EXPECT_TRUE(reply == unsubscribed("b", 1) + unsubscribed("c", 0) ||
                reply == unsubscribed("c", 1) + unsubscribed("b", 0))
        << reply;
    EXPECT_EQ(context.pubsub.channel_count(), 0u);  // empty channels are removed

    // Nothing left: Redis replies with a nil channel name.
    EXPECT_EQ(send(client, context, {"UNSUBSCRIBE"}), "*3\r\n$11\r\nunsubscribe\r\n$-1\r\n:0\r\n");
}

TEST(PubSub, SubscribedModeRefusesOtherCommands) {
    tachyon::ServerContext context;
    Connection client{Socket()};
    send(client, context, {"SUBSCRIBE", "a"});
    EXPECT_EQ(send(client, context, {"GET", "k"}), "-ERR Can't execute 'get" + kRefusal);
    EXPECT_EQ(send(client, context, {"PUBLISH", "a", "m"}),
              "-ERR Can't execute 'publish" + kRefusal);
    // Errors that Redis checks first still come first.
    EXPECT_EQ(send(client, context, {"GET"}),
              "-ERR wrong number of arguments for 'get' command\r\n");
    EXPECT_EQ(send(client, context, {"FLY"}),
              "-ERR unknown command 'FLY', with args beginning with: \r\n");
    // PING still works, as an array.
    EXPECT_EQ(send(client, context, {"PING"}), "*2\r\n$4\r\npong\r\n$0\r\n\r\n");
    EXPECT_EQ(send(client, context, {"PING", "x"}), "*2\r\n$4\r\npong\r\n$1\r\nx\r\n");
}

TEST(PubSub, LeavingTheLastChannelEndsSubscribedMode) {
    tachyon::ServerContext context;
    Connection client{Socket()};
    send(client, context, {"SUBSCRIBE", "a"});
    send(client, context, {"UNSUBSCRIBE", "a"});
    EXPECT_EQ(send(client, context, {"PING"}), "+PONG\r\n");
    EXPECT_EQ(send(client, context, {"SET", "k", "v"}), "+OK\r\n");
}

// The dangling-pointer case: a subscriber is deleted (it disconnected) while the channel still
// exists. With the sanitizers on, any pointer left behind would crash the PUBLISH below.
TEST(PubSub, DisconnectedSubscriberIsForgotten) {
    tachyon::ServerContext context;
    Connection stays{Socket()};
    Connection publisher{Socket()};
    send(stays, context, {"SUBSCRIBE", "news"});
    {
        Connection leaves{Socket()};
        send(leaves, context, {"SUBSCRIBE", "news", "only-me"});
        EXPECT_EQ(context.pubsub.channel_count(), 2u);
    }  // `leaves` is destroyed here, as the event loop does when a client hangs up
    EXPECT_EQ(context.pubsub.channel_count(), 1u);  // "only-me" had no one else and is gone
    EXPECT_EQ(send(publisher, context, {"PUBLISH", "news", "hi"}), ":1\r\n");
    EXPECT_EQ(send(publisher, context, {"PUBLISH", "only-me", "hi"}), ":0\r\n");
    EXPECT_EQ(stays.pending_reply, message("news", "hi"));
}

TEST(PubSub, ManySubscribersComeAndGo) {
    tachyon::ServerContext context;
    Connection publisher{Socket()};
    std::vector<std::unique_ptr<Connection>> subscribers;
    for (int i = 0; i < 100; ++i) {
        subscribers.push_back(std::make_unique<Connection>(Socket()));
        send(*subscribers.back(), context, {"SUBSCRIBE", "c", "d"});
    }
    // Delete every other one, from the middle of the list, as hang-ups would.
    for (std::size_t i = 0; i < subscribers.size(); i += 2) subscribers[i].reset();
    EXPECT_EQ(send(publisher, context, {"PUBLISH", "c", "x"}), ":50\r\n");
    subscribers.clear();
    EXPECT_EQ(context.pubsub.channel_count(), 0u);
    EXPECT_EQ(send(publisher, context, {"PUBLISH", "d", "x"}), ":0\r\n");
}

TEST(PubSub, QuitRepliesOkAndIgnoresTheRest) {
    tachyon::ServerContext context;
    Connection client{Socket()};
    tachyon::handle_input(client, context,
                          tachyon::encode_command({"QUIT"}) + tachyon::encode_command({"PING"}));
    EXPECT_EQ(client.pending_reply, "+OK\r\n");
    EXPECT_TRUE(client.closing);
}

TEST(PubSub, OnlySubscribersHaveAnOutputLimit) {
    tachyon::ServerContext context;
    Connection normal{Socket()};
    normal.pending_reply.assign(tachyon::kMaxSubscriberBacklogBytes + 1, 'x');
    EXPECT_FALSE(tachyon::over_output_limit(normal));

    Connection subscriber{Socket()};
    send(subscriber, context, {"SUBSCRIBE", "c"});
    subscriber.pending_reply.assign(tachyon::kMaxSubscriberBacklogBytes, 'x');
    EXPECT_FALSE(tachyon::over_output_limit(subscriber));
    subscriber.pending_reply += 'x';
    EXPECT_TRUE(tachyon::over_output_limit(subscriber));
}
