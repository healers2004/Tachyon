// LPUSH, RPUSH, LPOP, RPOP, LRANGE, LTRIM, LLEN.
#include "commands/command_util.h"
#include "protocol/resp_writer.h"

namespace tachyon {

namespace {

// LPUSH and RPUSH: create the list if needed, add every element, reply with the new length.
// LPUSH l a b c pushes a, then b, then c onto the front, leaving c b a.
std::string push(Store& store, const Args& args, std::int64_t now_ms, bool front) {
    bool wrong_type = false;
    List* list = find_or_create<List>(store, args[1], now_ms, wrong_type);
    if (wrong_type) return wrong_type_reply();
    for (std::size_t i = 2; i < args.size(); ++i) {
        if (front) {
            list->push_front(args[i]);
        } else {
            list->push_back(args[i]);
        }
    }
    return integer_reply(static_cast<long long>(list->size()));
}

// LPOP and RPOP key [count]. Without a count: one element, or nil. With a count: an array of
// up to `count` elements, or a nil array if the key is missing.
std::string pop(Store& store, const Args& args, std::int64_t now_ms, bool front) {
    if (args.size() > 3) return wrong_arg_count(front ? "lpop" : "rpop");
    const bool has_count = args.size() == 3;
    std::int64_t count = 1;
    if (has_count && (!parse_int(args[2], count) || count < 0)) {
        return error_reply("ERR value is out of range, must be positive");
    }

    bool wrong_type = false;
    List* list = find_as<List>(store, args[1], now_ms, wrong_type);
    if (wrong_type) return wrong_type_reply();
    if (list == nullptr) return has_count ? null_array_reply() : null_reply();

    std::size_t n = static_cast<std::size_t>(count) < list->size() ? static_cast<std::size_t>(count)
                                                                   : list->size();
    std::string reply = has_count ? array_header(n) : "";
    for (std::size_t i = 0; i < n; ++i) {
        if (front) {
            reply += bulk_reply(list->front());
            list->pop_front();
        } else {
            reply += bulk_reply(list->back());
            list->pop_back();
        }
    }
    if (list->empty()) store.del(args[1]);  // Redis deletes emptied lists
    return reply;
}

// Turns LRANGE/LTRIM's start and stop (negative counts from the end: -1 is the last element)
// into a clamped [first, last] range. Returns false if the range is empty.
bool resolve_range(std::int64_t start, std::int64_t stop, std::size_t size, std::size_t& first,
                   std::size_t& last) {
    const std::int64_t length = static_cast<std::int64_t>(size);
    if (start < 0) start += length;
    if (stop < 0) stop += length;
    if (start < 0) start = 0;
    if (start > stop || start >= length) return false;
    if (stop >= length) stop = length - 1;
    first = static_cast<std::size_t>(start);
    last = static_cast<std::size_t>(stop);
    return true;
}

}  // namespace

std::string cmd_lpush(Store& store, const Args& args, std::int64_t now_ms) {
    return push(store, args, now_ms, true);
}

std::string cmd_rpush(Store& store, const Args& args, std::int64_t now_ms) {
    return push(store, args, now_ms, false);
}

std::string cmd_lpop(Store& store, const Args& args, std::int64_t now_ms) {
    return pop(store, args, now_ms, true);
}

std::string cmd_rpop(Store& store, const Args& args, std::int64_t now_ms) {
    return pop(store, args, now_ms, false);
}

std::string cmd_lrange(Store& store, const Args& args, std::int64_t now_ms) {
    std::int64_t start = 0;
    std::int64_t stop = 0;
    if (!parse_int(args[2], start) || !parse_int(args[3], stop)) return not_integer_reply();

    bool wrong_type = false;
    const List* list = find_as<List>(store, args[1], now_ms, wrong_type);
    if (wrong_type) return wrong_type_reply();
    std::size_t first = 0;
    std::size_t last = 0;
    if (list == nullptr || !resolve_range(start, stop, list->size(), first, last)) {
        return array_header(0);
    }

    std::string reply = array_header(last - first + 1);
    // Iterators mark positions in the deque; begin() + n jumps straight to element n, and the
    // loop stops one past `last`.
    List::const_iterator end = list->begin() + static_cast<std::ptrdiff_t>(last) + 1;
    for (List::const_iterator it = list->begin() + static_cast<std::ptrdiff_t>(first); it != end;
         ++it) {
        reply += bulk_reply(*it);  // *it is the element at that position
    }
    return reply;
}

// Keeps only elements start..stop. Everything outside is erased; an empty result deletes the key.
std::string cmd_ltrim(Store& store, const Args& args, std::int64_t now_ms) {
    std::int64_t start = 0;
    std::int64_t stop = 0;
    if (!parse_int(args[2], start) || !parse_int(args[3], stop)) return not_integer_reply();

    bool wrong_type = false;
    List* list = find_as<List>(store, args[1], now_ms, wrong_type);
    if (wrong_type) return wrong_type_reply();
    if (list == nullptr) return simple_string_reply("OK");

    std::size_t first = 0;
    std::size_t last = 0;
    if (!resolve_range(start, stop, list->size(), first, last)) {
        store.del(args[1]);
        return simple_string_reply("OK");
    }
    // Erase the tail first, so the indexes of the front part don't move.
    list->erase(list->begin() + static_cast<std::ptrdiff_t>(last) + 1, list->end());
    list->erase(list->begin(), list->begin() + static_cast<std::ptrdiff_t>(first));
    return simple_string_reply("OK");
}

std::string cmd_llen(Store& store, const Args& args, std::int64_t now_ms) {
    bool wrong_type = false;
    const List* list = find_as<List>(store, args[1], now_ms, wrong_type);
    if (wrong_type) return wrong_type_reply();
    return integer_reply(list == nullptr ? 0 : static_cast<long long>(list->size()));
}

}  // namespace tachyon
