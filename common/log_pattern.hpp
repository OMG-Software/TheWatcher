#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace thewatcher
{

// Static safety check for log-monitor regular expressions (issue #169).
//
// The agent matches patterns with std::regex, which backtracks. A nested or
// overlapping quantifier such as "(a+)+$", "(a|aa)*c" or ".*.*.*z" takes
// exponential or high-polynomial time on a line that almost matches, and the
// search cannot be interrupted once it runs. The check is purely structural,
// so the server can reject a bad pattern at save time and the agent can refuse
// it again as defence in depth, both with the same rule.
//
// Rejected:
//  - back-references (\1..\9, \k): matching them is not regular and can be
//    exponential;
//  - a group that is itself repeated (*, +, {n,} or {n,m} with m > 1) and
//    contains a quantifier or an alternation, e.g. "(a+)+", "(\w+\s?)*", "(a|aa)*";
//  - two unbounded quantifiers in a row on the same atom or on ".", e.g.
//    ".*.*", "a*a*", "\d+\d+" (their matches overlap);
//  - more than max_log_pattern_unbounded unbounded quantifiers in one pattern;
//  - groups nested deeper than max_log_pattern_group_depth (stack use of
//    std::regex grows with nesting, issue #140).
// Plain literals, classes, alternation outside a repeated group, bounded
// repeats such as "\d{4}" and one or two ".*" between literals are accepted.
// The check is deliberately conservative: it may refuse an unusual but safe
// pattern, and it never needs to run a regex to decide.
constexpr int max_log_pattern_group_depth = 16;
constexpr int max_log_pattern_unbounded = 3;

namespace log_pattern_detail
{

    struct Quantifier
    {
        bool present = false;   // a quantifier follows the atom
        bool repeats = false;   // it can match the atom more than once
        bool unbounded = false; // it has no upper limit (*, +, {n,})
        std::size_t length = 0; // characters used, including a lazy "?"
    };

    // Parses an ECMAScript quantifier at pattern[i]. A "{" that does not form
    // a valid {n}, {n,} or {n,m} is not a quantifier.
    inline Quantifier parse_quantifier(const std::string& pattern, std::size_t i)
    {
        Quantifier q;
        if (i >= pattern.size())
            return q;
        const char ch = pattern[i];
        if (ch == '*' || ch == '+')
        {
            q = {true, true, true, 1};
        }
        else if (ch == '?')
        {
            q = {true, false, false, 1};
        }
        else if (ch == '{')
        {
            std::size_t j = i + 1;
            const auto read_number = [&](long& out) {
                const std::size_t begin = j;
                out = 0;
                while (j < pattern.size() && pattern[j] >= '0' && pattern[j] <= '9')
                {
                    out = std::min(out * 10 + (pattern[j] - '0'), 1000000L); // clamp, never overflows
                    ++j;
                }
                return j > begin;
            };
            long low = 0;
            long high = 0;
            if (!read_number(low))
                return q;
            bool unbounded = false;
            if (j < pattern.size() && pattern[j] == ',')
            {
                ++j;
                if (!read_number(high))
                    unbounded = true;
            }
            else
            {
                high = low;
            }
            if (j >= pattern.size() || pattern[j] != '}')
                return q;
            q = {true, unbounded || high > 1, unbounded, j - i + 1};
        }
        else
        {
            return q;
        }
        // A trailing "?" makes the quantifier lazy; it does not change its reach.
        if (i + q.length < pattern.size() && pattern[i + q.length] == '?')
            ++q.length;
        return q;
    }

    // One open group (the bottom frame is the whole pattern).
    struct Frame
    {
        std::size_t start = 0;
        bool has_quantifier = false;    // a quantifier occurs somewhere inside
        bool has_alternation = false;   // a "|" occurs somewhere inside
        std::string previous_unbounded; // text of the previous atom if it had an unbounded quantifier
    };

} // namespace log_pattern_detail

// Returns an empty string when the pattern passes, otherwise the reason it is
// refused. It does not check that the pattern compiles; std::regex does that.
inline std::string log_pattern_error(const std::string& pattern)
{
    using namespace log_pattern_detail;

    std::vector<Frame> frames(1);
    int unbounded_count = 0;

    // Registers one atom (text) with its quantifier in the current group.
    const auto note_atom = [&](const std::string& text, const Quantifier& q) -> std::string {
        Frame& frame = frames.back();
        if (q.present)
            frame.has_quantifier = true;
        if (q.unbounded)
        {
            if (++unbounded_count > max_log_pattern_unbounded)
                return "pattern has more than " + std::to_string(max_log_pattern_unbounded) +
                       " unbounded quantifiers (*, + or {n,})";
            const auto& previous = frame.previous_unbounded;
            if (!previous.empty() && (previous == text || previous == "." || text == "."))
                return "pattern repeats overlapping atoms back to back (for example \".*.*\")";
        }
        frame.previous_unbounded = q.unbounded ? text : std::string{};
        return {};
    };

    std::size_t i = 0;
    while (i < pattern.size())
    {
        const char ch = pattern[i];
        if (ch == '|')
        {
            frames.back().has_alternation = true;
            frames.back().previous_unbounded.clear();
            ++i;
        }
        else if (ch == '(')
        {
            if (static_cast<int>(frames.size()) > max_log_pattern_group_depth)
                return "pattern nests groups deeper than " + std::to_string(max_log_pattern_group_depth) + " levels";
            frames.emplace_back();
            frames.back().start = i;
            ++i;
            // Skip the group prefix: (?: (?= (?!
            if (i + 1 < pattern.size() && pattern[i] == '?' &&
                (pattern[i + 1] == ':' || pattern[i + 1] == '=' || pattern[i + 1] == '!'))
                i += 2;
        }
        else if (ch == ')' && frames.size() > 1)
        {
            const Frame closed = frames.back();
            frames.pop_back();
            ++i;
            const auto q = parse_quantifier(pattern, i);
            if (q.repeats && (closed.has_quantifier || closed.has_alternation))
                return "pattern repeats a group that itself contains a quantifier or alternation (for example "
                       "\"(a+)+\" or \"(a|aa)*\")";
            // Whatever the group contains counts for the enclosing group too.
            frames.back().has_quantifier = frames.back().has_quantifier || closed.has_quantifier;
            frames.back().has_alternation = frames.back().has_alternation || closed.has_alternation;
            const auto text = pattern.substr(closed.start, i - closed.start);
            if (auto error = note_atom(text, q); !error.empty())
                return error;
            i += q.length;
        }
        else
        {
            std::size_t length = 1;
            if (ch == '\\' && i + 1 < pattern.size())
            {
                const char next = pattern[i + 1];
                if ((next >= '1' && next <= '9') || next == 'k')
                    return "pattern uses a back-reference, which is not allowed";
                length = 2;
            }
            else if (ch == '[')
            {
                // ECMAScript: the class ends at the first unescaped "]".
                std::size_t j = i + 1;
                while (j < pattern.size() && pattern[j] != ']')
                    j += (pattern[j] == '\\' && j + 1 < pattern.size()) ? 2 : 1;
                length = std::min(j + 1, pattern.size()) - i;
            }
            const auto q = parse_quantifier(pattern, i + length);
            if (auto error = note_atom(pattern.substr(i, length), q); !error.empty())
                return error;
            i += length + q.length;
        }
    }
    return {};
}

} // namespace thewatcher
