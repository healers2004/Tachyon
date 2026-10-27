#include "server/server_commands.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "protocol/resp_writer.h"
#include "server/connection.h"

using tachyon::info_text;
using tachyon::ServerContext;

namespace {

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

TEST(Info, AllSectionsByDefault) {
    ServerContext context;
    context.options.port = 6390;
    const std::string text = info_text(context, {});
    for (const char* section :
         {"# Server", "# Clients", "# Persistence", "# Stats", "# Keyspace"}) {
        EXPECT_TRUE(contains(text, section)) << section;
    }
    EXPECT_TRUE(contains(text, "tachyon_version:1.0.0\r\n"));
    EXPECT_TRUE(contains(text, "tcp_port:6390\r\n"));
    EXPECT_TRUE(contains(text, "persistence_enabled:0\r\n"));
    EXPECT_TRUE(contains(text, "\r\n\r\n# Clients\r\n"));  // a blank line between sections
}

TEST(Info, OneSection) {
    ServerContext context;
    const std::string text = info_text(context, {"STATS"});
    EXPECT_EQ(text, "# Stats\r\ntotal_connections_received:0\r\ntotal_commands_processed:0\r\n");
    EXPECT_EQ(info_text(context, {"nonsense"}), "");
}

TEST(Info, CountsAndKeyspace) {
    ServerContext context;
    tachyon::Connection client{tachyon::Socket()};
    tachyon::handle_input(client, context,
                          tachyon::encode_command({"SET", "a", "1", "EX", "100"}) +
                              tachyon::encode_command({"SET", "b", "2"}) +
                              tachyon::encode_command({"GET", "a"}) +
                              tachyon::encode_command({"INFO", "keyspace"}));
    EXPECT_EQ(context.stats.commands_processed, 4u);
    EXPECT_EQ(context.stats.changes_since_save, 2u);
    const std::string keyspace = "# Keyspace\r\ndb0:keys=2,expires=1,avg_ttl=0\r\n";
    EXPECT_TRUE(contains(client.pending_reply, tachyon::bulk_reply(keyspace)))
        << client.pending_reply;
}
