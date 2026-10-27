// Pub/sub channels: which connections listen on which channel. Messages aren't stored. PUBLISH
// copies a message into every current subscriber's reply buffer, and a channel nobody listens
// on simply drops it, as in Redis.
#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace tachyon {

struct Connection;  // declared, not defined: this header only needs to point at connections

class PubSub {
public:
    PubSub() = default;
    // Connections keep a pointer back to this object, so it must stay where it is.
    PubSub(const PubSub&) = delete;
    PubSub& operator=(const PubSub&) = delete;

    // Each returns how many channels the connection is subscribed to afterwards. Subscribing
    // twice, or unsubscribing from a channel it isn't on, changes nothing.
    std::size_t subscribe(Connection& connection, const std::string& channel);
    std::size_t unsubscribe(Connection& connection, const std::string& channel);

    // Removes the connection from every channel. Connection's destructor calls this, so no
    // channel can keep pointing at a connection that no longer exists.
    void unsubscribe_all(Connection& connection);

    // Queues `message` for every subscriber of `channel`; returns how many received it.
    std::size_t publish(const std::string& channel, const std::string& message);

    std::size_t channel_count() const { return channels_.size(); }

private:
    // Removes one connection from one channel's set, and the channel once nobody is left.
    void remove(Connection& connection, const std::string& channel);

    // A map of sets: channel name -> the connections subscribed to it. The pointers don't own
    // anything; the event loop owns every Connection, and each one leaves its channels before
    // it's deleted.
    std::unordered_map<std::string, std::unordered_set<Connection*>> channels_;
};

}  // namespace tachyon
