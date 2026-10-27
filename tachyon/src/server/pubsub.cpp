#include "server/pubsub.h"

#include "protocol/resp_writer.h"
#include "server/connection.h"

namespace tachyon {

std::size_t PubSub::subscribe(Connection& connection, const std::string& channel) {
    connection.pubsub = this;  // so the connection can leave its channels when it's deleted
    // insert() returns {position, inserted}; .second is false if it was already subscribed.
    if (connection.channels.insert(channel).second) {
        channels_[channel].insert(&connection);  // [] creates an empty set for a new channel
    }
    return connection.channels.size();
}

std::size_t PubSub::unsubscribe(Connection& connection, const std::string& channel) {
    if (connection.channels.erase(channel) > 0) remove(connection, channel);
    return connection.channels.size();
}

void PubSub::unsubscribe_all(Connection& connection) {
    // This loop only reads connection.channels; it's cleared afterwards, never mid-loop.
    for (const std::string& channel : connection.channels) {
        remove(connection, channel);
    }
    connection.channels.clear();
}

std::size_t PubSub::publish(const std::string& channel, const std::string& message) {
    std::unordered_map<std::string, std::unordered_set<Connection*>>::iterator it =
        channels_.find(channel);
    if (it == channels_.end()) return 0;

    // Built once, copied to each subscriber: *3 "message" <channel> <message>.
    const std::string bytes =
        array_header(3) + bulk_reply("message") + bulk_reply(channel) + bulk_reply(message);
    for (Connection* subscriber : it->second) {
        subscriber->pending_reply += bytes;  // sent by the event loop's write pass
    }
    return it->second.size();
}

void PubSub::remove(Connection& connection, const std::string& channel) {
    std::unordered_map<std::string, std::unordered_set<Connection*>>::iterator it =
        channels_.find(channel);
    if (it == channels_.end()) return;
    it->second.erase(&connection);
    if (it->second.empty()) channels_.erase(it);  // don't keep empty channels around
}

}  // namespace tachyon
