#include "api.hpp"

#include "api_json.hpp"
#include "common/SingleLog.hpp"
#include "common/protocol.hpp"
#include "report_generator.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <sodium.h>

namespace thewatcher::server
{

using json = nlohmann::json;

// ── Construction ──────────────────────────────────────────────────────────────

ApiServer::ApiServer(Store& store, ZapHandler& zap, const std::string& host, int port, int64_t login_lockout_ms,
                     int64_t login_ip_window_ms)
    : store_(store)
    , zap_(zap)
    , host_(host)
    , port_(port)
    , login_lockout_ms_(login_lockout_ms)
    , login_ip_window_ms_(login_ip_window_ms)
    , http_(std::make_unique<httplib::Server>())
{
    LOG_FUNCTION_TRACE
    LOGF_DEBUG("Constructing API server host=%s port=%d", host_.c_str(), port_);
    setup_routes();
}

ApiServer::~ApiServer()
{
    stop();
}

void ApiServer::start()
{
    LOG_FUNCTION_TRACE
    thread_ = std::thread([this] {
        LOGF_INFO("Listening on %s:%d", host_.c_str(), port_);
        if (!http_->listen(host_.c_str(), port_))
            LOGF_ERROR("Failed to listen on %s:%d", host_.c_str(), port_);
    });
}

void ApiServer::stop()
{
    LOG_FUNCTION_TRACE
    http_->stop();
    if (thread_.joinable())
    {
        thread_.join();
        LOG_DEBUG("API server thread joined");
    }
}

void ApiServer::drain_commands(std::vector<PendingCommand>& out)
{
    std::lock_guard<std::mutex> lk(cmd_mutex_);
    const auto before = out.size();
    while (!cmd_queue_.empty())
    {
        out.push_back(std::move(cmd_queue_.front()));
        cmd_queue_.pop();
    }
    if (out.size() > before)
        LOGF_TRACE("Drained %zu pending API command(s)", out.size() - before);
}

// ── Route helpers ─────────────────────────────────────────────────────────────

namespace
{

    int64_t now_ms()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    json agent_to_json(const AgentRecord& a)
    {
        return {
            {"agent_id",             a.agent_id                          },
            {"hostname",             a.hostname                          },
            {"platform",             a.platform                          },
            {"curve_public_key_z85", a.curve_public_key_z85              },
            {"approved",             a.approved                          },
            {"rejected",             a.rejected                          },
            {"connected",            a.connected                         },
            {"maintenance",          a.maintenance                       },
            {"maintenance_reason",   a.maintenance_reason                },
            {"maintenance_until",    a.maintenance_until                 },
            {"collection_interval",  a.collection_interval               },
            {"heartbeat_interval",   a.heartbeat_interval                },
            {"process_limit",        a.process_limit                     },
            {"first_seen",           a.first_seen                        },
            {"last_seen",            a.last_seen                         },
            {"description",          a.description                       },
            {"runbook_markdown",     a.runbook_markdown                  },
            {"collector_config",     a.collector_config                  },
            {"thresholds",
             {{"cpu",
               {{"warning_pct_of_avg", a.cpu_warning_pct_of_avg},
                {"degraded_pct_of_avg", a.cpu_degraded_pct_of_avg},
                {"critical_pct_of_avg", a.cpu_critical_pct_of_avg}}},
              {"memory",
               {{"warning_pct_of_avg", a.memory_warning_pct_of_avg},
                {"degraded_pct_of_avg", a.memory_degraded_pct_of_avg},
                {"critical_pct_of_avg", a.memory_critical_pct_of_avg}}},
              {"disk",
               {{"warning_pct_of_avg", a.disk_warning_pct_of_avg},
                {"degraded_pct_of_avg", a.disk_degraded_pct_of_avg},
                {"critical_pct_of_avg", a.disk_critical_pct_of_avg}}},
              {"network",
               {{"warning_pct_of_avg", a.network_warning_pct_of_avg},
                {"degraded_pct_of_avg", a.network_degraded_pct_of_avg},
                {"critical_pct_of_avg", a.network_critical_pct_of_avg}}}}}
        };
    }

    void validate_thresholds(double warning, double degraded, double critical, const std::string& indicator)
    {
        if (!std::isfinite(warning) || !std::isfinite(degraded) || !std::isfinite(critical))
            throw std::runtime_error(indicator + " thresholds must be finite numbers");
        if (warning <= 0.0 || degraded <= 0.0 || critical <= 0.0)
            throw std::runtime_error(indicator + " thresholds must be greater than zero");
        if (!(warning < degraded && degraded < critical))
            throw std::runtime_error(indicator + " thresholds must be ordered warning < degraded < critical");
    }

    void validate_percent_thresholds(const PercentThresholds& thresholds, const std::string& label)
    {
        validate_thresholds(thresholds.warning_percent, thresholds.degraded_percent, thresholds.critical_percent,
                            label);
        if (thresholds.critical_percent > 100.0)
            throw std::runtime_error(label + " critical threshold must be less than or equal to 100");
    }

    void validate_network_thresholds(const NetworkThresholds& thresholds, const std::string& label)
    {
        validate_thresholds(thresholds.warning_mbps, thresholds.degraded_mbps, thresholds.critical_mbps, label);
    }

    void validate_collector_config(const CollectorConfig& config)
    {
        validate_percent_thresholds(config.cpu, "cpu");
        validate_percent_thresholds(config.memory, "memory");
        if (config.cpu_readings <= 0 || config.memory_readings <= 0 || config.disk_readings <= 0 ||
            config.network_readings <= 0 || config.process_readings <= 0)
            throw std::runtime_error("collector readings must be greater than zero");

        for (const auto& disk : config.disks)
        {
            if (disk.mount_point.empty())
                throw std::runtime_error("disk mount_point is required");
            validate_percent_thresholds(disk.thresholds, "disk " + disk.mount_point);
        }
        for (const auto& network : config.networks)
        {
            if (network.interface_name.empty())
                throw std::runtime_error("network interface_name is required");
            validate_network_thresholds(network.thresholds, "network " + network.interface_name);
        }
        for (const auto& process : config.processes)
        {
            if (process.name.empty())
                throw std::runtime_error("process name is required");
            if (process.expected_count <= 0)
                throw std::runtime_error("process expected_count must be greater than zero");
        }
        // SEC-10: log-monitor entries name files the agent will read whose
        // contents then surface on the dashboard — validate them strictly.
        for (const auto& log : config.logs)
        {
            if (log.path.empty())
                throw std::runtime_error("log path is required");
            if (log.path.size() > 4096)
                throw std::runtime_error("log path is too long");
            if (log.pattern.empty() || log.pattern.size() > 256)
                throw std::runtime_error("log pattern must be 1-256 characters");
            if (log.indicator_name.empty() || log.indicator_name.size() > 64)
                throw std::runtime_error("log indicator_name must be 1-64 characters");
            if (log.severity != "yellow" && log.severity != "amber" && log.severity != "red")
                throw std::runtime_error("log severity must be yellow, amber, or red");
        }
    }

    void read_indicator_thresholds(const json& thresholds, const std::string& indicator, double& warning,
                                   double& degraded, double& critical)
    {
        const auto& value = thresholds.at(indicator);
        warning = value.at("warning_pct_of_avg").get<double>();
        degraded = value.at("degraded_pct_of_avg").get<double>();
        critical = value.at("critical_pct_of_avg").get<double>();
        validate_thresholds(warning, degraded, critical, indicator);
    }

    AgentRecord apply_threshold_payload(AgentRecord rec, const json& body)
    {
        const auto& thresholds = body.at("thresholds");
        read_indicator_thresholds(thresholds, "cpu", rec.cpu_warning_pct_of_avg, rec.cpu_degraded_pct_of_avg,
                                  rec.cpu_critical_pct_of_avg);
        read_indicator_thresholds(thresholds, "memory", rec.memory_warning_pct_of_avg, rec.memory_degraded_pct_of_avg,
                                  rec.memory_critical_pct_of_avg);
        read_indicator_thresholds(thresholds, "disk", rec.disk_warning_pct_of_avg, rec.disk_degraded_pct_of_avg,
                                  rec.disk_critical_pct_of_avg);
        read_indicator_thresholds(thresholds, "network", rec.network_warning_pct_of_avg,
                                  rec.network_degraded_pct_of_avg, rec.network_critical_pct_of_avg);
        return rec;
    }

    json group_to_json(const GroupRecord& group)
    {
        return {
            {"group_id", group.group_id},
            {"name",     group.name    },
            {"built_in", group.built_in}
        };
    }

    json user_to_json(const UserRecord& user, const std::vector<int64_t>& group_ids)
    {
        return {
            {"user_id",   user.user_id },
            {"username",  user.username},
            {"role",      user.role    },
            {"built_in",  user.built_in},
            {"disabled",  user.disabled},
            {"group_ids", group_ids    }
        };
    }

    json alert_to_json(const AlertRecord& alert)
    {
        return {
            {"alert_id",         alert.alert_id        },
            {"agent_id",         alert.agent_id        },
            {"indicator",        alert.indicator       },
            {"old_status",       alert.old_status      },
            {"new_status",       alert.new_status      },
            {"message",          alert.message         },
            {"created_at",       alert.created_at      },
            {"acknowledged_by",  alert.acknowledged_by },
            {"acknowledged_at",  alert.acknowledged_at },
            {"deleted_at",       alert.deleted_at      },
            {"note",             alert.note            },
            {"escalated_at",     alert.escalated_at    },
            {"runbook_url",      alert.runbook_url     },
            {"runbook_markdown", alert.runbook_markdown}
        };
    }

    json runbook_to_json(const RunbookRecord& r)
    {
        return {
            {"runbook_id", r.runbook_id},
            {"agent_id",   r.agent_id  },
            {"indicator",  r.indicator },
            {"status",     r.status    },
            {"url",        r.url       },
            {"markdown",   r.markdown  },
            {"notes",      r.notes     },
            {"created_by", r.created_by},
            {"created_at", r.created_at}
        };
    }

    json maintenance_window_to_json(const MaintenanceWindowRecord& w)
    {
        return {
            {"window_id",  w.window_id },
            {"agent_id",   w.agent_id  },
            {"start_ms",   w.start_ms  },
            {"end_ms",     w.end_ms    },
            {"reason",     w.reason    },
            {"created_by", w.created_by},
            {"created_at", w.created_at}
        };
    }

    json silence_to_json(const SilenceRecord& s)
    {
        return {
            {"silence_id", s.silence_id},
            {"agent_id",   s.agent_id  },
            {"indicator",  s.indicator },
            {"reason",     s.reason    },
            {"until_ms",   s.until_ms  },
            {"created_by", s.created_by},
            {"created_at", s.created_at}
        };
    }

    json status_history_to_json(const StatusHistoryRow& h)
    {
        return {
            {"id",         h.id        },
            {"agent_id",   h.agent_id  },
            {"indicator",  h.indicator },
            {"old_status", h.old_status},
            {"new_status", h.new_status},
            {"message",    h.message   },
            {"created_at", h.created_at}
        };
    }

    std::string cookie_value(const httplib::Request& req, const std::string& name)
    {
        auto cookie = req.get_header_value("Cookie");
        const auto needle = name + "=";
        auto pos = cookie.find(needle);
        if (pos == std::string::npos)
            return {};
        pos += needle.size();
        auto end = cookie.find(';', pos);
        return cookie.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    }

    std::string random_token()
    {
        std::array<unsigned char, 32> bytes{};
        randombytes_buf(bytes.data(), bytes.size());
        std::array<char, 65> token{};
        sodium_bin2hex(token.data(), token.size(), bytes.data(), bytes.size());
        return token.data();
    }

    // Cryptographically random 16-hex-char command ID (H-3).
    std::string random_command_id()
    {
        std::array<unsigned char, 8> bytes{};
        randombytes_buf(bytes.data(), bytes.size());
        std::array<char, 17> token{};
        sodium_bin2hex(token.data(), token.size(), bytes.data(), bytes.size());
        return token.data();
    }

    // Reject bodies where Content-Type is present but is not application/json (L-3).
    bool is_json_content_type(const httplib::Request& req)
    {
        const auto ct = req.get_header_value("Content-Type");
        if (ct.empty())
            return true; // tolerate missing header for backward compatibility
        return ct.find("application/json") != std::string::npos;
    }

    bool is_unsafe_method(const std::string& method)
    {
        return method == "POST" || method == "PUT" || method == "PATCH" || method == "DELETE";
    }

    std::string lowercase_ascii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }

    bool contains_forbidden_authority_char(const std::string& authority)
    {
        return std::any_of(authority.begin(), authority.end(), [](unsigned char ch) {
            return std::isspace(ch) || ch == '/' || ch == '\\' || ch == '@';
        });
    }

    std::string trim_ascii(std::string_view value)
    {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
            value.remove_prefix(1);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
            value.remove_suffix(1);
        return std::string(value);
    }

    bool parse_port(std::string_view text, int& port)
    {
        if (text.empty() || !std::all_of(text.begin(), text.end(), [](unsigned char ch) {
                return std::isdigit(ch);
            }))
        {
            return false;
        }

        int parsed = 0;
        const auto* first = text.data();
        const auto* last = text.data() + text.size();
        const auto [ptr, ec] = std::from_chars(first, last, parsed);
        if (ec != std::errc{} || ptr != last || parsed < 1 || parsed > 65535)
            return false;

        port = parsed;
        return true;
    }

    int default_port_for_scheme(const std::string& scheme)
    {
        if (scheme == "http")
            return 80;
        if (scheme == "https")
            return 443;
        return 0;
    }

    struct AuthorityParts
    {
        std::string host;
        int port = 0;
        bool has_port = false;
    };

    struct OriginParts
    {
        std::string scheme;
        std::string host;
        int port = 0;
    };

    std::optional<AuthorityParts> parse_authority(const std::string& authority)
    {
        if (authority.empty() || contains_forbidden_authority_char(authority))
            return std::nullopt;

        AuthorityParts parsed;
        if (authority.front() == '[')
        {
            const auto close = authority.find(']');
            if (close == std::string::npos)
                return std::nullopt;
            parsed.host = lowercase_ascii(authority.substr(1, close - 1));
            if (parsed.host.empty())
                return std::nullopt;
            if (close + 1 < authority.size())
            {
                if (authority[close + 1] != ':' ||
                    !parse_port(std::string_view(authority).substr(close + 2), parsed.port))
                {
                    return std::nullopt;
                }
                parsed.has_port = true;
            }
            return parsed;
        }

        const auto first_colon = authority.find(':');
        const auto last_colon = authority.rfind(':');
        if (first_colon != last_colon)
            return std::nullopt;
        const auto host = first_colon == std::string::npos ? authority : authority.substr(0, first_colon);
        if (host.empty())
            return std::nullopt;
        parsed.host = lowercase_ascii(host);
        if (first_colon != std::string::npos)
        {
            if (!parse_port(std::string_view(authority).substr(first_colon + 1), parsed.port))
                return std::nullopt;
            parsed.has_port = true;
        }
        return parsed;
    }

    std::optional<OriginParts> parse_origin(const std::string& origin)
    {
        const auto scheme_end = origin.find("://");
        if (scheme_end == std::string::npos)
            return std::nullopt;

        OriginParts parsed;
        parsed.scheme = lowercase_ascii(origin.substr(0, scheme_end));
        const auto default_port = default_port_for_scheme(parsed.scheme);
        if (default_port == 0)
            return std::nullopt;

        const auto authority_start = scheme_end + 3;
        const auto authority_end = origin.find_first_of("/?#", authority_start);
        const auto authority = origin.substr(
            authority_start, authority_end == std::string::npos ? std::string::npos : authority_end - authority_start);
        const auto parsed_authority = parse_authority(authority);
        if (!parsed_authority)
            return std::nullopt;

        parsed.host = parsed_authority->host;
        parsed.port = parsed_authority->has_port ? parsed_authority->port : default_port;
        return parsed;
    }

    std::optional<std::string> request_scheme(const httplib::Request& req)
    {
        auto forwarded = req.get_header_value("X-Forwarded-Proto");
        if (forwarded.empty())
            return std::string("http");

        const auto comma = forwarded.find(',');
        forwarded = lowercase_ascii(
            trim_ascii(std::string_view(forwarded).substr(0, comma == std::string::npos ? forwarded.size() : comma)));
        if (forwarded == "http" || forwarded == "https")
            return forwarded;
        return std::nullopt;
    }

    std::optional<OriginParts> request_origin(const httplib::Request& req)
    {
        const auto scheme = request_scheme(req);
        if (!scheme)
            return std::nullopt;

        const auto parsed_authority = parse_authority(req.get_header_value("Host"));
        if (!parsed_authority)
            return std::nullopt;

        const auto default_port = default_port_for_scheme(*scheme);
        if (default_port == 0)
            return std::nullopt;

        return OriginParts{
            *scheme,
            parsed_authority->host,
            parsed_authority->has_port ? parsed_authority->port : default_port,
        };
    }

    bool is_loopback_host(const std::string& host)
    {
        return host == "localhost" || host == "localhost." || host == "::1" || host.rfind("127.", 0) == 0;
    }

    bool allows_browser_origin(const httplib::Request& req, const std::string& origin)
    {
        const auto request_parts = parse_origin(origin);
        const auto api_parts = request_origin(req);
        if (!request_parts || !api_parts)
            return false;

        const auto same_origin = request_parts->scheme == api_parts->scheme && request_parts->host == api_parts->host &&
                                 request_parts->port == api_parts->port;
        const auto loopback_dashboard = request_parts->scheme == api_parts->scheme &&
                                        is_loopback_host(request_parts->host) && is_loopback_host(api_parts->host);
        return same_origin || loopback_dashboard;
    }

    void apply_browser_origin_headers(const httplib::Request& req, httplib::Response& res)
    {
        const auto origin = req.get_header_value("Origin");
        if (origin.empty() || !allows_browser_origin(req, origin))
            return;

        res.set_header("Access-Control-Allow-Origin", origin);
        res.set_header("Access-Control-Allow-Credentials", "true");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.set_header("Access-Control-Allow-Methods", "GET,POST,PUT,DELETE,OPTIONS");
        res.set_header("Vary", "Origin");
    }

    bool rejects_unsafe_browser_origin(const httplib::Request& req)
    {
        if (!is_unsafe_method(req.method))
            return false;

        auto origin = req.get_header_value("Origin");
        if (origin.empty())
            origin = req.get_header_value("Referer");
        if (origin.empty())
            return false;

        return !allows_browser_origin(req, origin);
    }

    std::string hash_password(const std::string& password)
    {
        char hash[crypto_pwhash_STRBYTES] = {};
        if (crypto_pwhash_str(hash, password.c_str(), password.size(), crypto_pwhash_OPSLIMIT_INTERACTIVE,
                              crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0)
            throw std::runtime_error("password hashing failed");
        return hash;
    }

    std::string normalize_role(const std::string& role)
    {
        if (role == "admin")
            return "global_admin";
        if (role == "operator")
            return "global_operator";
        if (role == "viewer")
            return "global_viewer";
        return role;
    }

    bool is_valid_role(const std::string& role)
    {
        const auto normalized = normalize_role(role);
        return normalized == "global_admin" || normalized == "global_operator" || normalized == "global_viewer" ||
               normalized == "group_admin" || normalized == "group_operator" || normalized == "group_viewer";
    }

    bool is_global_role(const std::string& role)
    {
        const auto normalized = normalize_role(role);
        return normalized == "global_admin" || normalized == "global_operator" || normalized == "global_viewer";
    }

    bool can_operate(const std::string& role)
    {
        const auto normalized = normalize_role(role);
        return normalized == "global_admin" || normalized == "global_operator" || normalized == "group_admin" ||
               normalized == "group_operator";
    }

    bool can_administer_users(const std::string& role)
    {
        const auto normalized = normalize_role(role);
        return normalized == "global_admin" || normalized == "group_admin";
    }

    bool can_manage_groups(const std::string& role)
    {
        return normalize_role(role) == "global_admin";
    }

    std::unordered_set<int64_t> user_group_set(Store& store, int64_t user_id)
    {
        std::unordered_set<int64_t> groups;
        for (const auto group_id : store.get_user_groups(user_id))
            groups.insert(group_id);
        return groups;
    }

    bool group_visible_to_session(Store& store, const SessionRecord& session, int64_t group_id)
    {
        if (is_global_role(session.role))
            return true;
        if (group_id <= 0)
            return false;
        return user_group_set(store, session.user_id).contains(group_id);
    }

    void set_json(httplib::Response& res, const json& body)
    {
        res.set_content(body.dump(), "application/json");
    }

} // namespace

std::optional<SessionRecord> ApiServer::current_session(const httplib::Request& req)
{
    auto token = cookie_value(req, "tw_session");
    if (token.empty())
        return std::nullopt;
    return store_.get_session(token, now_ms());
}

std::optional<SessionRecord> ApiServer::require_role(const httplib::Request& req, httplib::Response& res,
                                                     const std::string& role)
{
    if (rejects_unsafe_browser_origin(req))
    {
        res.status = 403;
        set_json(res, json{
                          {"error", "cross-origin request rejected"}
        });
        return std::nullopt;
    }

    auto session = current_session(req);
    if (!session)
    {
        res.status = 401;
        set_json(res, json{
                          {"error", "authentication required"}
        });
        return std::nullopt;
    }
    session->role = normalize_role(session->role);

    const auto needed = normalize_role(role);
    const bool allowed =
        needed == "viewer" || needed == "global_viewer" || needed == "group_viewer" ? is_valid_role(session->role)
        : needed == "operator" || needed == "global_operator" || needed == "group_operator" ? can_operate(session->role)
        : needed == "admin" || needed == "global_admin" || needed == "group_admin" ? can_administer_users(session->role)
                                                                                   : false;
    if (!allowed)
    {
        res.status = 403;
        set_json(res, json{
                          {"error", "permission denied"}
        });
        return std::nullopt;
    }
    return session;
}

bool ApiServer::can_access_agent(const SessionRecord& session, const std::string& agent_id)
{
    if (is_global_role(session.role))
        return true;

    std::unordered_set<int64_t> user_groups;
    for (auto group_id : store_.get_user_groups(session.user_id))
        user_groups.insert(group_id);
    if (user_groups.empty())
        return false;

    for (auto group_id : store_.get_agent_groups(agent_id))
    {
        if (user_groups.contains(group_id))
            return true;
    }
    return false;
}

std::optional<SessionRecord> ApiServer::require_agent_access(const httplib::Request& req, httplib::Response& res,
                                                             const std::string& role, const std::string& agent_id)
{
    auto session = require_role(req, res, role);
    if (!session)
        return std::nullopt;
    if (!can_access_agent(*session, agent_id))
    {
        res.status = 403;
        set_json(res, json{
                          {"error", "agent is outside the user's groups"}
        });
        return std::nullopt;
    }
    return session;
}

std::string ApiServer::enqueue_simple_command(const std::string& id, CommandType ct)
{
    CommandMessage cmd;
    cmd.command_id = random_command_id();
    cmd.command_type = static_cast<uint8_t>(ct);
    std::lock_guard<std::mutex> lk(cmd_mutex_);
    cmd_queue_.push({id, cmd});
    return cmd.command_id;
}

// ── Routes ────────────────────────────────────────────────────────────────────

void ApiServer::setup_routes()
{
    LOG_FUNCTION_TRACE
    http_->set_default_headers({
        {"Cache-Control",           "no-store"                                  },
        {"Content-Security-Policy", "default-src 'none'; frame-ancestors 'none'"},
        {"Referrer-Policy",         "no-referrer"                               },
        {"X-Content-Type-Options",  "nosniff"                                   },
        {"X-Frame-Options",         "DENY"                                      }
    });
    LOG_DEBUG("Default HTTP security headers configured");

    http_->Options(R"(.*)", [](const httplib::Request& req, httplib::Response& res) {
        res.status = 204;
        apply_browser_origin_headers(req, res);
    });
    http_->set_post_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        apply_browser_origin_headers(req, res);
    });

    http_->Post("/api/login", [this](const httplib::Request& req, httplib::Response& res) {
        try
        {
            if (!is_json_content_type(req))
            {
                res.status = 415;
                set_json(res, json{
                                  {"error", "Content-Type must be application/json"}
                });
                return;
            }
            auto body = json::parse(req.body);
            const auto username = body.at("username").get<std::string>();
            const auto password = body.at("password").get<std::string>();

            // M-2: Reject absurdly long credentials before hashing.
            if (username.size() > 64 || password.size() > 256)
            {
                res.status = 400;
                set_json(res, json{
                                  {"error", "username or password too long"}
                });
                return;
            }

            // M-1 + SEC-12: login rate limiting. A fixed per-username window
            // that RESETS on expiry (previously the lockout could never
            // re-arm, allowing unlimited guessing after the first 15
            // minutes) plus a per-source-IP throttle across all usernames.
            // Expired entries are pruned so both maps stay bounded.
            constexpr int max_failures = 5;
            constexpr int max_ip_failures = 10;
            const auto now = now_ms();
            const std::string remote = req.remote_addr;
            {
                std::lock_guard<std::mutex> lk(login_mutex_);

                // Prune expired windows: an entry whose window has elapsed
                // is removed, which also implements the fixed-window reset.
                for (auto it = login_failures_.begin(); it != login_failures_.end();)
                {
                    if (now - it->second.second >= login_lockout_ms_)
                        it = login_failures_.erase(it);
                    else
                        ++it;
                }
                for (auto it = login_ip_failures_.begin(); it != login_ip_failures_.end();)
                {
                    if (now - it->second.second >= login_ip_window_ms_)
                        it = login_ip_failures_.erase(it);
                    else
                        ++it;
                }

                auto& failures = login_failures_[username];
                if (failures.first >= max_failures)
                {
                    res.status = 429;
                    res.set_header("Retry-After", "900");
                    set_json(res, json{
                                      {"error", "too many failed login attempts; try again later"}
                    });
                    return;
                }

                // SEC-12: throttle by source IP across all usernames so a
                // single host cannot spray many accounts (or lock them out).
                auto& ip_failures = login_ip_failures_[remote];
                if (ip_failures.first >= max_ip_failures)
                {
                    res.status = 429;
                    res.set_header("Retry-After", "60");
                    set_json(res, json{
                                      {"error", "too many login attempts from this address; try again later"}
                    });
                    return;
                }
            }

            auto user = store_.get_user_by_username(username);
            if (!user || user->disabled ||
                crypto_pwhash_str_verify(user->password_hash.c_str(), password.c_str(), password.size()) != 0)
            {
                {
                    std::lock_guard<std::mutex> lk(login_mutex_);
                    auto& [count, window_start] = login_failures_[username];
                    if (count == 0)
                        window_start = now;
                    ++count;

                    auto& [ip_count, ip_window_start] = login_ip_failures_[remote];
                    if (ip_count == 0)
                        ip_window_start = now;
                    ++ip_count;
                }
                res.status = 401;
                set_json(res, json{
                                  {"error", "invalid credentials"}
                });
                return;
            }

            {
                std::lock_guard<std::mutex> lk(login_mutex_);
                login_failures_.erase(username);
            }

            SessionRecord session;
            session.token = random_token();
            session.user_id = user->user_id;
            session.username = user->username;
            session.role = user->role;
            session.created_at = now;
            session.expires_at = now + (8LL * 60LL * 60LL * 1000LL);
            store_.create_session(session);
            res.set_header("Set-Cookie", "tw_session=" + session.token +
                                             "; Path=/; HttpOnly; SameSite=Strict; Secure; Max-Age=28800");
            set_json(res, json{
                              {"username", user->username            },
                              {"role",     normalize_role(user->role)}
            });
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            set_json(res, json{
                              {"error", e.what()}
            });
        }
    });

    http_->Post("/api/logout", [this](const httplib::Request& req, httplib::Response& res) {
        if (rejects_unsafe_browser_origin(req))
        {
            res.status = 403;
            set_json(res, json{
                              {"error", "cross-origin request rejected"}
            });
            return;
        }

        auto token = cookie_value(req, "tw_session");
        if (!token.empty())
            store_.delete_session(token);
        res.set_header("Set-Cookie", "tw_session=; Path=/; HttpOnly; SameSite=Strict; Secure; Max-Age=0");
        set_json(res, json{
                          {"ok", true}
        });
    });

    http_->Get("/api/session", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = current_session(req);
        if (!session)
        {
            res.status = 401;
            set_json(res, json{
                              {"error", "authentication required"}
            });
            return;
        }
        set_json(res, json{
                          {"username", session->username            },
                          {"role",     normalize_role(session->role)}
        });
    });

    // ── Agents ────────────────────────────────────────────────────────────────

    http_->Get("/api/groups", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = require_role(req, res, "viewer");
        if (!session)
            return;
        auto groups = store_.list_groups();
        json arr = json::array();
        for (const auto& group : groups)
        {
            if (group_visible_to_session(store_, *session, group.group_id))
                arr.push_back(group_to_json(group));
        }
        set_json(res, arr);
    });

    http_->Post("/api/groups", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = require_role(req, res, "viewer");
        if (!session)
            return;
        if (!can_manage_groups(session->role))
        {
            res.status = 403;
            set_json(res, json{
                              {"error", "permission denied"}
            });
            return;
        }
        try
        {
            if (!is_json_content_type(req))
            {
                res.status = 415;
                set_json(res, json{
                                  {"error", "Content-Type must be application/json"}
                });
                return;
            }
            auto body = json::parse(req.body);
            const auto name = body.at("name").get<std::string>();
            if (name.empty() || name.size() > 64)
                throw std::runtime_error("group name must be between 1 and 64 characters");
            auto group_id = store_.create_group(name);
            set_json(res, json{
                              {"group_id", group_id}
            });
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            set_json(res, json{
                              {"error", e.what()}
            });
        }
    });

    http_->Get("/api/users", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = require_role(req, res, "viewer");
        if (!session)
            return;
        auto users = store_.list_users();
        const auto session_groups = store_.get_user_groups(session->user_id);
        json arr = json::array();
        for (const auto& user : users)
        {
            const auto user_groups = store_.get_user_groups(user.user_id);
            bool visible = is_global_role(session->role);
            for (const auto group_id : user_groups)
            {
                visible = visible ||
                          std::find(session_groups.begin(), session_groups.end(), group_id) != session_groups.end();
            }
            if (visible || user.user_id == session->user_id)
                arr.push_back(user_to_json(user, user_groups));
        }
        set_json(res, arr);
    });

    http_->Post("/api/users", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = require_role(req, res, "viewer");
        if (!session)
            return;
        if (!can_administer_users(session->role))
        {
            res.status = 403;
            set_json(res, json{
                              {"error", "permission denied"}
            });
            return;
        }
        try
        {
            if (!is_json_content_type(req))
            {
                res.status = 415;
                set_json(res, json{
                                  {"error", "Content-Type must be application/json"}
                });
                return;
            }
            auto body = json::parse(req.body);
            const auto username = body.at("username").get<std::string>();
            const auto password = body.at("password").get<std::string>();
            const auto role = body.value("role", "group_viewer");
            if (username.empty() || password.empty())
                throw std::runtime_error("username and password are required");
            if (username.size() > 64)
                throw std::runtime_error("username must be 64 characters or fewer");
            if (password.size() > 256)
                throw std::runtime_error("password must be 256 characters or fewer");
            if (!is_valid_role(role))
                throw std::runtime_error("invalid role");

            std::vector<int64_t> group_ids;
            if (body.contains("group_ids"))
                group_ids = body.at("group_ids").get<std::vector<int64_t>>();
            if (!is_global_role(session->role))
            {
                const auto allowed_groups = store_.get_user_groups(session->user_id);
                for (const auto group_id : group_ids)
                {
                    if (std::find(allowed_groups.begin(), allowed_groups.end(), group_id) == allowed_groups.end())
                        throw std::runtime_error("group admins can only assign users to their own groups");
                }
                const auto normalized = normalize_role(role);
                if (normalized == "global_admin" || normalized == "global_operator" || normalized == "global_viewer")
                    throw std::runtime_error("group admins can only create group-scoped users");
            }

            const auto user_id = store_.create_user(username, hash_password(password), normalize_role(role));
            store_.set_user_groups(user_id, group_ids);
            LOGF_INFO("Created user username=%s role=%s group_count=%zu", username.c_str(), role.c_str(),
                      group_ids.size());
            set_json(res, json{
                              {"user_id", user_id}
            });
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            set_json(res, json{
                              {"error", e.what()}
            });
        }
    });

    http_->Get("/api/alerts", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = require_role(req, res, "viewer");
        if (!session)
            return;
        const bool include_archived = req.get_param_value("include_archived") == "1";
        auto alerts = store_.list_alerts(include_archived);
        json arr = json::array();
        for (const auto& alert : alerts)
        {
            if (can_access_agent(*session, alert.agent_id))
            {
                auto item = alert;
                if (auto agent = store_.get_agent(alert.agent_id))
                    item.runbook_markdown = agent->runbook_markdown;
                arr.push_back(alert_to_json(item));
            }
        }
        set_json(res, arr);
    });

    http_->Get("/api/alerts/unacknowledged", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = require_role(req, res, "viewer");
        if (!session)
            return;
        auto alerts = store_.list_unacknowledged_alerts();
        json arr = json::array();
        for (const auto& alert : alerts)
        {
            if (can_access_agent(*session, alert.agent_id))
            {
                auto item = alert;
                if (auto agent = store_.get_agent(alert.agent_id))
                    item.runbook_markdown = agent->runbook_markdown;
                arr.push_back(alert_to_json(item));
            }
        }
        set_json(res, arr);
    });

    http_->Post("/api/alerts/:id/ack", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = require_role(req, res, "operator");
        if (!session)
            return;
        const auto alert_id = std::stoll(req.path_params.at("id"));
        auto allowed = false;
        for (const auto& alert : store_.list_alerts(false))
            allowed = allowed || (alert.alert_id == alert_id && can_access_agent(*session, alert.agent_id));
        if (!allowed)
        {
            res.status = 403;
            set_json(res, json{
                              {"error", "alert is outside the user's groups"}
            });
            return;
        }
        std::string note;
        if (!req.body.empty())
        {
            try
            {
                const auto body = json::parse(req.body);
                if (body.contains("note") && body["note"].is_string())
                {
                    note = body["note"].get<std::string>();
                    if (note.size() > 4096)
                        note.resize(4096);
                }
            }
            catch (...)
            {
            }
        }
        store_.acknowledge_alert(alert_id, session->username, now_ms(), note);
        set_json(res, json{
                          {"ok", true}
        });
    });

    http_->Post("/api/alerts/bulk-ack", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = require_role(req, res, "operator");
        if (!session)
            return;
        if (req.body.empty())
        {
            res.status = 400;
            set_json(res, json{
                              {"error", "body required"}
            });
            return;
        }
        json body;
        try
        {
            body = json::parse(req.body);
        }
        catch (...)
        {
            res.status = 400;
            set_json(res, json{
                              {"error", "invalid JSON"}
            });
            return;
        }

        if (!body.contains("alert_ids") || !body["alert_ids"].is_array())
        {
            res.status = 400;
            set_json(res, json{
                              {"error", "alert_ids array required"}
            });
            return;
        }
        std::string note = (body.contains("note") && body["note"].is_string()) ? body["note"].get<std::string>() : "";
        if (note.size() > 4096)
            note.resize(4096);
        const auto all_alerts = store_.list_alerts(false);
        std::vector<int64_t> allowed_ids;
        for (const auto& id_val : body["alert_ids"])
        {
            if (!id_val.is_number_integer())
                continue;
            const auto id = id_val.get<int64_t>();
            for (const auto& alert : all_alerts)
            {
                if (alert.alert_id == id && can_access_agent(*session, alert.agent_id))
                {
                    allowed_ids.push_back(id);
                    break;
                }
            }
        }
        store_.bulk_acknowledge_alerts(allowed_ids, session->username, now_ms(), note);
        set_json(res, json{
                          {"ok",    true              },
                          {"count", allowed_ids.size()}
        });
    });

    http_->Post("/api/alerts/bulk-archive", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = require_role(req, res, "operator");
        if (!session)
            return;
        if (req.body.empty())
        {
            res.status = 400;
            set_json(res, json{
                              {"error", "body required"}
            });
            return;
        }
        json body;
        try
        {
            body = json::parse(req.body);
        }
        catch (...)
        {
            res.status = 400;
            set_json(res, json{
                              {"error", "invalid JSON"}
            });
            return;
        }

        if (!body.contains("alert_ids") || !body["alert_ids"].is_array())
        {
            res.status = 400;
            set_json(res, json{
                              {"error", "alert_ids array required"}
            });
            return;
        }
        const auto all_alerts = store_.list_alerts(false);
        std::vector<int64_t> allowed_ids;
        for (const auto& id_val : body["alert_ids"])
        {
            if (!id_val.is_number_integer())
                continue;
            const auto id = id_val.get<int64_t>();
            for (const auto& alert : all_alerts)
            {
                if (alert.alert_id == id && can_access_agent(*session, alert.agent_id))
                {
                    allowed_ids.push_back(id);
                    break;
                }
            }
        }
        store_.bulk_soft_delete_alerts(allowed_ids, now_ms());
        set_json(res, json{
                          {"ok",    true              },
                          {"count", allowed_ids.size()}
        });
    });

    http_->Delete("/api/alerts/:id", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = require_role(req, res, "operator");
        if (!session)
            return;
        const auto alert_id = std::stoll(req.path_params.at("id"));
        auto allowed = false;
        for (const auto& alert : store_.list_alerts(false))
            allowed = allowed || (alert.alert_id == alert_id && can_access_agent(*session, alert.agent_id));
        if (!allowed)
        {
            res.status = 403;
            set_json(res, json{
                              {"error", "alert is outside the user's groups"}
            });
            return;
        }
        store_.soft_delete_alert(alert_id, now_ms());
        set_json(res, json{
                          {"ok", true}
        });
    });

    http_->Get("/api/pending-enrollments", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = require_role(req, res, "operator");
        if (!session)
            return;
        if (!is_global_role(session->role))
        {
            res.status = 403;
            set_json(res, json{
                              {"error", "permission denied"}
            });
            return;
        }
        auto agents = store_.list_pending_agents();
        json arr = json::array();
        for (auto& a : agents)
            arr.push_back(agent_to_json(a));
        set_json(res, arr);
    });

    // GET /api/agents — list approved agents
    http_->Get("/api/agents", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = require_role(req, res, "viewer");
        if (!session)
            return;
        try
        {
            auto agents = store_.list_approved_agents();
            LOGF_TRACE("GET /api/agents returned %zu agent(s)", agents.size());
            json arr = json::array();
            for (auto& a : agents)
            {
                if (!can_access_agent(*session, a.agent_id))
                    continue;
                auto item = agent_to_json(a);
                item["group_ids"] = store_.get_agent_groups(a.agent_id);
                arr.push_back(item);
            }
            res.set_content(arr.dump(), "application/json");
        }
        catch (const std::exception& e)
        {
            LOGF_WARNING("GET /api/agents failed: %s", e.what());
            res.status = 500;
            res.set_content(
                json{
                    {"error", e.what()}
            }
                    .dump(),
                "application/json");
        }
    });

    // POST /api/agents/:id/approve — approve an agent
    http_->Post("/api/agents/:id/approve", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = require_role(req, res, "operator");
        if (!session)
            return;
        if (!is_global_role(session->role))
        {
            res.status = 403;
            set_json(res, json{
                              {"error", "permission denied"}
            });
            return;
        }
        try
        {
            auto id = req.path_params.at("id");
            LOGF_INFO("Approving agent id=%s", id.c_str());
            store_.approve_agent(id);
            std::vector<int64_t> group_ids;
            if (!req.body.empty())
            {
                auto body = json::parse(req.body);
                if (body.contains("group_ids"))
                    group_ids = body["group_ids"].get<std::vector<int64_t>>();
            }
            if (group_ids.empty())
            {
                auto groups = store_.list_groups();
                for (const auto& group : groups)
                {
                    if (group.name == "Admins")
                    {
                        group_ids.push_back(group.group_id);
                        break;
                    }
                }
            }
            store_.set_agent_groups(id, group_ids);
            auto rec = store_.get_agent(id);
            if (rec)
            {
                // SEC-04: bind the key to the agent it belongs to so ZAP can
                // report the authenticated identity for the data plane.
                zap_.add_key(rec->curve_public_key_z85, rec->agent_id);
                LOGF_DEBUG("Approved agent key added to ZAP id=%s", id.c_str());
            }
            res.set_content(
                json{
                    {"ok", true}
            }
                    .dump(),
                "application/json");
        }
        catch (const std::exception& e)
        {
            LOGF_WARNING("POST /api/agents/:id/approve failed: %s", e.what());
            res.status = 500;
            res.set_content(
                json{
                    {"error", e.what()}
            }
                    .dump(),
                "application/json");
        }
    });

    // POST /api/agents/:id/reject — reject an agent enrollment
    http_->Post("/api/agents/:id/reject", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = require_role(req, res, "operator");
        if (!session)
            return;
        if (!is_global_role(session->role))
        {
            res.status = 403;
            set_json(res, json{
                              {"error", "permission denied"}
            });
            return;
        }
        try
        {
            auto id = req.path_params.at("id");
            LOGF_INFO("Rejecting agent id=%s", id.c_str());
            auto rec = store_.get_agent(id);
            if (rec)
            {
                zap_.remove_key(rec->curve_public_key_z85);
                LOGF_DEBUG("Rejected agent key removed from ZAP id=%s", id.c_str());
            }
            store_.reject_agent(id);
            res.set_content(
                json{
                    {"ok", true}
            }
                    .dump(),
                "application/json");
        }
        catch (const std::exception& e)
        {
            LOGF_WARNING("POST /api/agents/:id/reject failed: %s", e.what());
            res.status = 500;
            res.set_content(
                json{
                    {"error", e.what()}
            }
                    .dump(),
                "application/json");
        }
    });

    // DELETE /api/agents/:id — remove agent
    http_->Post("/api/agents/:id/groups", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = require_role(req, res, "operator");
        if (!session)
            return;
        if (!is_global_role(session->role))
        {
            res.status = 403;
            set_json(res, json{
                              {"error", "permission denied"}
            });
            return;
        }
        try
        {
            auto id = req.path_params.at("id");
            auto body = json::parse(req.body);
            auto group_ids = body.at("group_ids").get<std::vector<int64_t>>();
            store_.set_agent_groups(id, group_ids);
            LOGF_INFO("Updated agent groups id=%s group_count=%zu", id.c_str(), group_ids.size());
            set_json(res, json{
                              {"ok", true}
            });
        }
        catch (const std::exception& e)
        {
            LOGF_WARNING("POST /api/agents/:id/groups failed: %s", e.what());
            res.status = 400;
            set_json(res, json{
                              {"error", e.what()}
            });
        }
    });

    http_->Delete("/api/agents/:id", [this](const httplib::Request& req, httplib::Response& res) {
        try
        {
            auto id = req.path_params.at("id");
            auto session = require_agent_access(req, res, "operator", id);
            if (!session)
                return;
            LOGF_INFO("Deleting agent id=%s", id.c_str());
            auto rec = store_.get_agent(id);
            if (rec)
            {
                zap_.remove_key(rec->curve_public_key_z85);
                LOGF_DEBUG("Deleted agent key removed from ZAP id=%s", id.c_str());
            }
            store_.delete_agent(id);
            res.set_content(
                json{
                    {"ok", true}
            }
                    .dump(),
                "application/json");
        }
        catch (const std::exception& e)
        {
            LOGF_WARNING("DELETE /api/agents/:id failed: %s", e.what());
            res.status = 500;
            res.set_content(
                json{
                    {"error", e.what()}
            }
                    .dump(),
                "application/json");
        }
    });

    // ── Metrics ───────────────────────────────────────────────────────────────

    // GET /api/metrics — latest snapshot for every agent
    http_->Get("/api/metrics", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = require_role(req, res, "viewer");
        if (!session)
            return;
        try
        {
            auto rows = store_.latest_metrics();
            LOGF_TRACE("GET /api/metrics returned %zu latest row(s)", rows.size());
            json arr = json::array();
            for (auto& r : rows)
            {
                if (!can_access_agent(*session, r.agent_id))
                    continue;
                arr.push_back({
                    {"agent_id",     r.agent_id                                                    },
                    {"timestamp_ms", r.timestamp_ms                                                },
                    {"metrics",      json(thewatcher::proto::unpack<SystemMetrics>(r.metrics_cbor))}
                });
            }
            res.set_content(arr.dump(), "application/json");
        }
        catch (const std::exception& e)
        {
            LOGF_WARNING("GET /api/metrics failed: %s", e.what());
            res.status = 500;
            res.set_content(
                json{
                    {"error", e.what()}
            }
                    .dump(),
                "application/json");
        }
    });

    // GET /api/metrics/:id?limit=N — history for one agent
    http_->Get("/api/metrics/:id", [this](const httplib::Request& req, httplib::Response& res) {
        try
        {
            auto id = req.path_params.at("id");
            if (!require_agent_access(req, res, "viewer", id))
                return;
            int limit = 100;
            if (req.has_param("limit"))
                limit = std::stoi(req.get_param_value("limit"));
            limit = std::max(1, std::min(limit, 1000));
            auto rows = store_.get_metrics(id, limit);
            LOGF_TRACE("GET /api/metrics/%s returned %zu row(s) with limit=%d", id.c_str(), rows.size(), limit);
            json arr = json::array();
            for (auto& r : rows)
            {
                arr.push_back({
                    {"agent_id",     r.agent_id                                                    },
                    {"timestamp_ms", r.timestamp_ms                                                },
                    {"metrics",      json(thewatcher::proto::unpack<SystemMetrics>(r.metrics_cbor))}
                });
            }
            res.set_content(arr.dump(), "application/json");
        }
        catch (const std::exception& e)
        {
            LOGF_WARNING("GET /api/metrics/:id failed: %s", e.what());
            res.status = 500;
            res.set_content(
                json{
                    {"error", e.what()}
            }
                    .dump(),
                "application/json");
        }
    });

    // ── Commands ──────────────────────────────────────────────────────────────

    // POST /api/agents/:id/command
    // Body: { "command_type": <int>, "args": <base64 or omit> }
    http_->Post("/api/agents/:id/command", [this](const httplib::Request& req, httplib::Response& res) {
        try
        {
            auto id = req.path_params.at("id");
            if (!require_agent_access(req, res, "operator", id))
                return;
            auto body = json::parse(req.body);

            CommandMessage cmd;
            cmd.command_id = random_command_id();
            cmd.command_type = body.at("command_type").get<uint8_t>();
            // M-7: Validate against known CommandType values.
            static constexpr uint8_t valid_command_types[] = {
                static_cast<uint8_t>(CommandType::SET_INTERVAL),
                static_cast<uint8_t>(CommandType::SET_PROCESS_LIMIT),
                static_cast<uint8_t>(CommandType::RESTART_COLLECTORS),
                static_cast<uint8_t>(CommandType::PAUSE),
                static_cast<uint8_t>(CommandType::RESUME),
                static_cast<uint8_t>(CommandType::GET_STATUS),
                static_cast<uint8_t>(CommandType::DISCONNECT),
            };
            bool valid_type = false;
            for (auto t : valid_command_types)
                valid_type = valid_type || (t == cmd.command_type);
            if (!valid_type)
                throw std::runtime_error("unknown command_type");
            // args is optional raw bytes packed by caller; default empty
            if (body.contains("args"))
            {
                cmd.args = body["args"].get<std::vector<uint8_t>>();
                if (cmd.args.size() > 4096)
                    throw std::runtime_error("args payload too large (max 4096 bytes)");
            }

            {
                std::lock_guard<std::mutex> lk(cmd_mutex_);
                cmd_queue_.push({id, cmd});
            }
            LOGF_DEBUG("Queued command id=%s agent=%s type=%u args_size=%zu", cmd.command_id.c_str(), id.c_str(),
                       static_cast<unsigned>(cmd.command_type), cmd.args.size());
            res.set_content(
                json{
                    {"ok",         true          },
                    {"command_id", cmd.command_id}
            }
                    .dump(),
                "application/json");
        }
        catch (const std::exception& e)
        {
            LOGF_WARNING("POST /api/agents/:id/command failed: %s", e.what());
            res.status = 400;
            res.set_content(
                json{
                    {"error", e.what()}
            }
                    .dump(),
                "application/json");
        }
    });

    // POST /api/agents/:id/set_interval
    http_->Post("/api/agents/:id/set_interval", [this](const httplib::Request& req, httplib::Response& res) {
        try
        {
            auto id = req.path_params.at("id");
            if (!require_agent_access(req, res, "operator", id))
                return;
            auto body = json::parse(req.body);
            int secs = body.at("interval_seconds").get<int>();
            if (secs <= 0)
                throw std::runtime_error("interval_seconds must be greater than zero");
            LOGF_INFO("Setting collection interval id=%s interval_seconds=%d", id.c_str(), secs);

            auto rec = store_.get_agent(id);
            if (rec)
            {
                rec->collection_interval = secs;
                store_.upsert_agent(*rec);
            }

            SetIntervalArgs args{secs};
            CommandMessage cmd;
            cmd.command_id = random_command_id();
            cmd.command_type = static_cast<uint8_t>(CommandType::SET_INTERVAL);
            cmd.args = proto::pack(args);

            {
                std::lock_guard<std::mutex> lk(cmd_mutex_);
                cmd_queue_.push({id, cmd});
            }
            LOGF_DEBUG("Queued SET_INTERVAL command id=%s agent=%s interval_seconds=%d", cmd.command_id.c_str(),
                       id.c_str(), secs);
            res.set_content(
                json{
                    {"ok", true}
            }
                    .dump(),
                "application/json");
        }
        catch (const std::exception& e)
        {
            LOGF_WARNING("POST /api/agents/:id/set_interval failed: %s", e.what());
            res.status = 400;
            res.set_content(
                json{
                    {"error", e.what()}
            }
                    .dump(),
                "application/json");
        }
    });

    // POST /api/agents/:id/set_process_limit
    http_->Post("/api/agents/:id/set_process_limit", [this](const httplib::Request& req, httplib::Response& res) {
        try
        {
            auto id = req.path_params.at("id");
            if (!require_agent_access(req, res, "operator", id))
                return;
            auto body = json::parse(req.body);
            int limit = body.at("limit").get<int>();
            if (limit <= 0)
                throw std::runtime_error("limit must be greater than zero");
            LOGF_INFO("Setting process limit id=%s limit=%d", id.c_str(), limit);

            auto rec = store_.get_agent(id);
            if (rec)
            {
                rec->process_limit = limit;
                store_.upsert_agent(*rec);
            }

            SetProcessLimitArgs args{limit};
            CommandMessage cmd;
            cmd.command_id = random_command_id();
            cmd.command_type = static_cast<uint8_t>(CommandType::SET_PROCESS_LIMIT);
            cmd.args = proto::pack(args);

            {
                std::lock_guard<std::mutex> lk(cmd_mutex_);
                cmd_queue_.push({id, cmd});
            }
            LOGF_DEBUG("Queued SET_PROCESS_LIMIT command id=%s agent=%s limit=%d", cmd.command_id.c_str(), id.c_str(),
                       limit);
            res.set_content(
                json{
                    {"ok", true}
            }
                    .dump(),
                "application/json");
        }
        catch (const std::exception& e)
        {
            LOGF_WARNING("POST /api/agents/:id/set_process_limit failed: %s", e.what());
            res.status = 400;
            res.set_content(
                json{
                    {"error", e.what()}
            }
                    .dump(),
                "application/json");
        }
    });

    http_->Post("/api/agents/:id/thresholds", [this](const httplib::Request& req, httplib::Response& res) {
        try
        {
            auto id = req.path_params.at("id");
            if (!require_agent_access(req, res, "operator", id))
                return;

            auto rec = store_.get_agent(id);
            if (!rec)
            {
                res.status = 404;
                set_json(res, json{
                                  {"error", "agent not found"}
                });
                return;
            }

            auto body = json::parse(req.body);
            auto updated = apply_threshold_payload(*rec, body);
            store_.set_agent_thresholds(updated);
            LOGF_INFO("Updated per-agent thresholds id=%s", id.c_str());
            set_json(res, json{
                              {"ok", true}
            });
        }
        catch (const std::exception& e)
        {
            LOGF_WARNING("POST /api/agents/:id/thresholds failed: %s", e.what());
            res.status = 400;
            set_json(res, json{
                              {"error", e.what()}
            });
        }
    });

    http_->Post("/api/agents/:id/collector_config", [this](const httplib::Request& req, httplib::Response& res) {
        try
        {
            auto id = req.path_params.at("id");
            auto session = require_agent_access(req, res, "operator", id);
            if (!session)
                return;

            auto rec = store_.get_agent(id);
            if (!rec)
            {
                res.status = 404;
                set_json(res, json{
                                  {"error", "agent not found"}
                });
                return;
            }

            auto body = json::parse(req.body);
            const auto interval = body.at("collection_interval").get<int>();
            const auto heartbeat_interval = body.value("heartbeat_interval", rec->heartbeat_interval);
            const auto process_limit = body.at("process_limit").get<int>();
            if (interval <= 0)
                throw std::runtime_error("collection_interval must be greater than zero");
            if (heartbeat_interval < 1 || heartbeat_interval > 60)
                throw std::runtime_error("heartbeat_interval must be between 1 and 60");
            if (process_limit <= 0)
                throw std::runtime_error("process_limit must be greater than zero");

            auto config = body.at("collector_config").get<CollectorConfig>();
            validate_collector_config(config);

            // SEC-10: a log path makes the agent read a local file and put
            // its contents on the dashboard — only global admins may set
            // them. Operators keep full access to every other collector
            // setting.
            if (!config.logs.empty() && session->role != "global_admin")
            {
                LOGF_WARNING("Non-admin attempted to configure log monitor paths user=%s role=%s",
                             session->username.c_str(), session->role.c_str());
                res.status = 403;
                set_json(res, json{
                                  {"error", "log monitor paths can only be configured by a global admin"}
                });
                return;
            }

            rec->collection_interval = interval;
            rec->heartbeat_interval = heartbeat_interval;
            rec->process_limit = process_limit;
            rec->collector_config = config;
            if (body.contains("runbook_markdown"))
                rec->runbook_markdown = body.at("runbook_markdown").get<std::string>().substr(0, 65536);
            store_.upsert_agent(*rec);
            store_.set_agent_collector_config(id, config);
            if (body.contains("group_ids"))
            {
                auto group_ids = body.at("group_ids").get<std::vector<int64_t>>();
                if (is_global_role(session->role))
                    store_.set_agent_groups(id, group_ids);
            }
            LOGF_INFO("Updated collector config id=%s interval=%d heartbeat_interval=%d process_limit=%d "
                      "disk_configs=%zu network_configs=%zu "
                      "process_watches=%zu",
                      id.c_str(), interval, heartbeat_interval, process_limit, config.disks.size(),
                      config.networks.size(), config.processes.size());
            set_json(res, json{
                              {"ok", true}
            });
        }
        catch (const std::exception& e)
        {
            LOGF_WARNING("POST /api/agents/:id/collector_config failed: %s", e.what());
            res.status = 400;
            set_json(res, json{
                              {"error", e.what()}
            });
        }
    });

    http_->Post("/api/agents/:id/maintenance", [this](const httplib::Request& req, httplib::Response& res) {
        try
        {
            auto id = req.path_params.at("id");
            if (!require_agent_access(req, res, "operator", id))
                return;
            auto body = req.body.empty() ? json::object() : json::parse(req.body);
            const auto reason = body.value("reason", "");
            const auto until = body.value("until_ms", static_cast<int64_t>(0));
            store_.set_agent_maintenance(id, true, reason, until);
            store_.clear_active_alerts_for_agent(id, now_ms());
            enqueue_simple_command(id, CommandType::PAUSE);
            set_json(res, json{
                              {"ok", true}
            });
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            set_json(res, json{
                              {"error", e.what()}
            });
        }
    });

    // Simple action commands (pause, resume, restart_collectors, disconnect, get_status)
    auto simple_cmd = [this](CommandType ct) {
        return [this, ct](const httplib::Request& req, httplib::Response& res) {
            try
            {
                auto id = req.path_params.at("id");
                if (!require_agent_access(req, res, "operator", id))
                    return;
                if (ct == CommandType::PAUSE)
                {
                    store_.set_agent_maintenance(id, true, "command", 0);
                    store_.clear_active_alerts_for_agent(id, now_ms());
                }
                else if (ct == CommandType::RESUME)
                {
                    store_.set_agent_maintenance(id, false, "", 0);
                }
                auto command_id = enqueue_simple_command(id, ct);
                LOGF_DEBUG("Queued simple command id=%s agent=%s type=%u", command_id.c_str(), id.c_str(),
                           static_cast<unsigned>(ct));
                res.set_content(
                    json{
                        {"ok", true}
                }
                        .dump(),
                    "application/json");
            }
            catch (const std::exception& e)
            {
                LOGF_WARNING("POST simple agent command failed: %s", e.what());
                res.status = 400;
                res.set_content(
                    json{
                        {"error", e.what()}
                }
                        .dump(),
                    "application/json");
            }
        };
    };

    http_->Post("/api/agents/:id/pause", simple_cmd(CommandType::PAUSE));
    http_->Post("/api/agents/:id/resume", simple_cmd(CommandType::RESUME));
    http_->Post("/api/agents/:id/restart_collectors", simple_cmd(CommandType::RESTART_COLLECTORS));
    http_->Post("/api/agents/:id/disconnect", simple_cmd(CommandType::DISCONNECT));
    http_->Post("/api/agents/:id/get_status", simple_cmd(CommandType::GET_STATUS));

    // GET /api/agents/:id/log-matches?limit=N
    http_->Get("/api/agents/:id/log-matches", [this](const httplib::Request& req, httplib::Response& res) {
        try
        {
            auto id = req.path_params.at("id");
            if (!require_agent_access(req, res, "viewer", id))
                return;
            int limit = 200;
            if (req.has_param("limit"))
            {
                const auto val = std::stoi(req.get_param_value("limit"));
                if (val > 0 && val <= 1000)
                    limit = val;
            }
            auto rows = store_.list_log_matches(id, limit);
            json arr = json::array();
            for (const auto& r : rows)
            {
                arr.push_back({
                    {"match_id",       r.match_id      },
                    {"agent_id",       r.agent_id      },
                    {"indicator_name", r.indicator_name},
                    {"path",           r.path          },
                    {"matched_line",   r.matched_line  },
                    {"severity",       r.severity      },
                    {"created_at",     r.created_at    },
                });
            }
            res.set_content(arr.dump(), "application/json");
        }
        catch (const std::exception& e)
        {
            LOGF_WARNING("GET /api/agents/:id/log-matches failed: %s", e.what());
            res.status = 500;
            res.set_content(R"({"error":"internal error"})", "application/json");
        }
    });

    // GET /api/uptime/:id?days=N — uptime percentage for one agent
    http_->Get("/api/uptime/:id", [this](const httplib::Request& req, httplib::Response& res) {
        try
        {
            auto id = req.path_params.at("id");
            if (!require_agent_access(req, res, "viewer", id))
                return;
            auto agent = store_.get_agent(id);
            if (!agent)
            {
                res.status = 404;
                set_json(res, json{
                                  {"error", "agent not found"}
                });
                return;
            }
            int days = 7;
            if (req.has_param("days"))
                days = std::max(1, std::min(90, std::stoi(req.get_param_value("days"))));
            const int64_t now = now_ms();
            const int64_t since = now - (static_cast<int64_t>(days) * 86400LL * 1000LL);
            const int64_t actual = store_.count_metrics_in_window(id, since, now);
            const int64_t window_ms = now - std::max(since, agent->first_seen);
            const int64_t expected =
                window_ms > 0 ? window_ms / (static_cast<int64_t>(agent->collection_interval) * 1000LL) : 0;
            const double pct =
                expected > 0 ? std::min(100.0, (static_cast<double>(actual) / static_cast<double>(expected)) * 100.0)
                             : 0.0;
            json resp_body = json::object();
            resp_body["agent_id"] = id;
            resp_body["days"] = days;
            resp_body["uptime_percent"] = pct;
            resp_body["actual_samples"] = actual;
            resp_body["expected_samples"] = expected;
            set_json(res, resp_body);
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            set_json(res, json{
                              {"error", e.what()}
            });
        }
    });

    // GET /api/maintenance-windows — list all maintenance windows
    http_->Get("/api/maintenance-windows", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = require_role(req, res, "viewer");
        if (!session)
            return;
        auto windows = store_.list_maintenance_windows();
        json arr = json::array();
        for (const auto& w : windows)
        {
            if (is_global_role(session->role) || (w.agent_id != "*" && can_access_agent(*session, w.agent_id)))
                arr.push_back(maintenance_window_to_json(w));
        }
        set_json(res, arr);
    });

    // POST /api/maintenance-windows — create a maintenance window
    http_->Post("/api/maintenance-windows", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = require_role(req, res, "operator");
        if (!session)
            return;
        try
        {
            if (!is_json_content_type(req))
            {
                res.status = 415;
                set_json(res, json{
                                  {"error", "Content-Type must be application/json"}
                });
                return;
            }
            auto body = json::parse(req.body);
            MaintenanceWindowRecord rec;
            rec.agent_id = body.value("agent_id", std::string("*"));
            rec.start_ms = body.at("start_ms").get<int64_t>();
            rec.end_ms = body.at("end_ms").get<int64_t>();
            rec.reason = body.value("reason", std::string(""));
            if (!is_global_role(session->role))
            {
                if (rec.agent_id == "*" || !can_access_agent(*session, rec.agent_id))
                {
                    res.status = 403;
                    set_json(res, json{
                                      {"error", "maintenance window is outside the user's groups"}
                    });
                    return;
                }
            }
            if (rec.reason.size() > 1024)
                throw std::runtime_error("reason must be 1024 characters or fewer");
            rec.created_by = session->username;
            rec.created_at = now_ms();
            if (rec.end_ms <= rec.start_ms)
                throw std::runtime_error("end_ms must be after start_ms");
            auto window_id = store_.create_maintenance_window(rec);
            set_json(res, json{
                              {"window_id", window_id}
            });
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            set_json(res, json{
                              {"error", e.what()}
            });
        }
    });

    // DELETE /api/maintenance-windows/:id — delete a maintenance window
    http_->Delete("/api/maintenance-windows/:id", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = require_role(req, res, "operator");
        if (!session)
            return;
        try
        {
            const auto window_id = std::stoll(req.path_params.at("id"));
            if (!is_global_role(session->role))
            {
                bool allowed = false;
                for (const auto& window : store_.list_maintenance_windows())
                {
                    if (window.window_id == window_id && window.agent_id != "*" &&
                        can_access_agent(*session, window.agent_id))
                    {
                        allowed = true;
                        break;
                    }
                }
                if (!allowed)
                {
                    res.status = 403;
                    set_json(res, json{
                                      {"error", "maintenance window is outside the user's groups"}
                    });
                    return;
                }
            }
            store_.delete_maintenance_window(window_id);
            set_json(res, json{
                              {"ok", true}
            });
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            set_json(res, json{
                              {"error", e.what()}
            });
        }
    });

    // POST /api/agents/:id/description — set agent description
    http_->Post("/api/agents/:id/description", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = require_role(req, res, "operator");
        if (!session)
            return;
        try
        {
            const auto id = req.path_params.at("id");
            if (!require_agent_access(req, res, "operator", id))
                return;
            if (!is_json_content_type(req))
            {
                res.status = 415;
                set_json(res, json{
                                  {"error", "Content-Type must be application/json"}
                });
                return;
            }
            const auto body = json::parse(req.body);
            const auto description = body.value("description", std::string(""));
            if (description.size() > 4096)
                throw std::runtime_error("description must be 4096 characters or fewer");
            store_.set_agent_description(id, description);
            LOGF_INFO("Set agent description agent_id=%s by=%s", id.c_str(), session->username.c_str());
            set_json(res, json{
                              {"ok", true}
            });
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            set_json(res, json{
                              {"error", e.what()}
            });
        }
    });

    // GET /api/settings — return configurable server settings (admin only)
    http_->Post("/api/agents/:id/runbook", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = require_role(req, res, "operator");
        if (!session)
            return;
        try
        {
            const auto id = req.path_params.at("id");
            if (!require_agent_access(req, res, "operator", id))
                return;
            if (!is_json_content_type(req))
            {
                res.status = 415;
                set_json(res, json{
                                  {"error", "Content-Type must be application/json"}
                });
                return;
            }
            const auto body = json::parse(req.body);
            const auto markdown = body.value("markdown", std::string(""));
            if (markdown.size() > 65536)
                throw std::runtime_error("markdown must be 65536 characters or fewer");
            store_.set_agent_runbook(id, markdown);
            LOGF_INFO("Set agent runbook agent_id=%s by=%s", id.c_str(), session->username.c_str());
            set_json(res, json{
                              {"ok", true}
            });
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            set_json(res, json{
                              {"error", e.what()}
            });
        }
    });

    http_->Get("/api/settings", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = require_role(req, res, "operator");
        if (!session)
            return;
        if (!is_global_role(session->role))
        {
            res.status = 403;
            set_json(res, json{
                              {"error", "permission denied"}
            });
            return;
        }
        json out = json::object();
        out["webhook_url"] = store_.get_setting("notifications.webhook_url", "");
        out["offline_after_seconds"] = std::stoi(store_.get_setting("offline_after_seconds", "120"));
        out["escalation_timeout_seconds"] = std::stoi(store_.get_setting("escalation_timeout_seconds", "3600"));
        out["metrics_retention_days"] = std::stoi(store_.get_setting("metrics_retention_days", "30"));
        out["reports_enabled"] = store_.get_setting("reports.enabled", "false") == "true";
        out["reports_schedule"] = store_.get_setting("reports.schedule", "daily");
        out["reports_hour"] = std::stoi(store_.get_setting("reports.hour", "8"));
        out["reports_day_of_week"] = std::stoi(store_.get_setting("reports.day_of_week", "1"));
        out["reports_webhook_url"] = store_.get_setting("reports.webhook_url", "");
        set_json(res, out);
    });

    // PUT /api/settings — update configurable server settings (admin only)
    http_->Put("/api/settings", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = require_role(req, res, "operator");
        if (!session)
            return;
        if (!is_global_role(session->role))
        {
            res.status = 403;
            set_json(res, json{
                              {"error", "permission denied"}
            });
            return;
        }
        try
        {
            const auto body = json::parse(req.body);
            if (body.contains("webhook_url"))
                store_.set_setting("notifications.webhook_url", body.at("webhook_url").get<std::string>());
            if (body.contains("offline_after_seconds"))
            {
                const auto v = body.at("offline_after_seconds").get<int>();
                if (v < 10 || v > 86400)
                    throw std::runtime_error("offline_after_seconds must be between 10 and 86400");
                store_.set_setting("offline_after_seconds", std::to_string(v));
            }
            if (body.contains("escalation_timeout_seconds"))
            {
                const auto v = body.at("escalation_timeout_seconds").get<int>();
                if (v < 60 || v > 604800)
                    throw std::runtime_error("escalation_timeout_seconds must be between 60 and 604800");
                store_.set_setting("escalation_timeout_seconds", std::to_string(v));
            }
            if (body.contains("metrics_retention_days"))
            {
                const auto v = body.at("metrics_retention_days").get<int>();
                if (v < 1 || v > 365)
                    throw std::runtime_error("metrics_retention_days must be between 1 and 365");
                store_.set_setting("metrics_retention_days", std::to_string(v));
            }
            if (body.contains("reports_enabled"))
                store_.set_setting("reports.enabled", body.at("reports_enabled").get<bool>() ? "true" : "false");
            if (body.contains("reports_schedule"))
            {
                const auto v = body.at("reports_schedule").get<std::string>();
                if (v != "daily" && v != "weekly")
                    throw std::runtime_error("reports_schedule must be 'daily' or 'weekly'");
                store_.set_setting("reports.schedule", v);
            }
            if (body.contains("reports_hour"))
            {
                const auto v = body.at("reports_hour").get<int>();
                if (v < 0 || v > 23)
                    throw std::runtime_error("reports_hour must be between 0 and 23");
                store_.set_setting("reports.hour", std::to_string(v));
            }
            if (body.contains("reports_day_of_week"))
            {
                const auto v = body.at("reports_day_of_week").get<int>();
                if (v < 0 || v > 6)
                    throw std::runtime_error("reports_day_of_week must be between 0 and 6");
                store_.set_setting("reports.day_of_week", std::to_string(v));
            }
            if (body.contains("reports_webhook_url"))
                store_.set_setting("reports.webhook_url",
                                   body.at("reports_webhook_url").get<std::string>().substr(0, 2048));
            set_json(res, json{
                              {"ok", true}
            });
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            set_json(res, json{
                              {"error", e.what()}
            });
        }
    });

    // POST /api/reports/send — immediately generate and send a fleet digest (admin only)
    http_->Post("/api/reports/send", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = require_role(req, res, "operator");
        if (!session)
            return;
        if (!is_global_role(session->role))
        {
            res.status = 403;
            set_json(res, json{
                              {"error", "permission denied"}
            });
            return;
        }
        const auto ts = now_ms();
        const auto payload = build_report_json(store_, ts);
        const bool sent = generate_and_send_report(store_, ts);
        json out;
        out["sent"] = sent;
        out["report"] = payload;
        set_json(res, out);
    });

    // PUT /api/users/:id/disable — disable a user account (admin only, not built-in)
    http_->Put("/api/users/:id/disable", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = require_role(req, res, "viewer");
        if (!session)
            return;
        if (normalize_role(session->role) != "global_admin")
        {
            res.status = 403;
            set_json(res, json{
                              {"error", "permission denied"}
            });
            return;
        }
        try
        {
            const auto user_id = std::stoll(req.path_params.at("id"));
            const auto affected = store_.disable_user(user_id);
            if (affected == 0)
            {
                // SEC-14: unknown id or the built_in guard refused the change;
                // a silent {"ok":true} hid both cases.
                res.status = 404;
                set_json(res, json{
                                  {"error", "user not found or built-in"}
                });
                return;
            }
            LOGF_INFO("Disabled user user_id=%lld", static_cast<long long>(user_id));
            set_json(res, json{
                              {"ok", true}
            });
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            set_json(res, json{
                              {"error", e.what()}
            });
        }
    });

    // PUT /api/users/:id/enable — re-enable a user account (admin only)
    http_->Put("/api/users/:id/enable", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = require_role(req, res, "viewer");
        if (!session)
            return;
        if (normalize_role(session->role) != "global_admin")
        {
            res.status = 403;
            set_json(res, json{
                              {"error", "permission denied"}
            });
            return;
        }
        try
        {
            const auto user_id = std::stoll(req.path_params.at("id"));
            store_.enable_user(user_id);
            LOGF_INFO("Enabled user user_id=%lld", static_cast<long long>(user_id));
            set_json(res, json{
                              {"ok", true}
            });
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            set_json(res, json{
                              {"error", e.what()}
            });
        }
    });

    // DELETE /api/users/:id — delete a user (admin only, not built-in)
    http_->Delete("/api/users/:id", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = require_role(req, res, "viewer");
        if (!session)
            return;
        if (normalize_role(session->role) != "global_admin")
        {
            res.status = 403;
            set_json(res, json{
                              {"error", "permission denied"}
            });
            return;
        }
        try
        {
            const auto user_id = std::stoll(req.path_params.at("id"));
            const auto affected = store_.delete_user(user_id);
            if (affected == 0)
            {
                // SEC-14: unknown id or the built_in guard refused the change.
                res.status = 404;
                set_json(res, json{
                                  {"error", "user not found or built-in"}
                });
                return;
            }
            LOGF_INFO("Deleted user user_id=%lld", static_cast<long long>(user_id));
            set_json(res, json{
                              {"ok", true}
            });
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            set_json(res, json{
                              {"error", e.what()}
            });
        }
    });

    // PUT /api/users/:id/password — change a user's password (admin changes any, user changes own)
    http_->Put("/api/users/:id/password", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = require_role(req, res, "viewer");
        if (!session)
            return;
        try
        {
            const auto user_id = std::stoll(req.path_params.at("id"));
            const auto target_user = [&]() -> std::optional<UserRecord> {
                for (const auto& u : store_.list_users())
                    if (u.user_id == user_id)
                        return u;
                return std::nullopt;
            }();
            if (!target_user)
                throw std::runtime_error("user not found");
            // Non-admins can only change their own password
            if (normalize_role(session->role) != "global_admin" && target_user->username != session->username)
            {
                res.status = 403;
                set_json(res, json{
                                  {"error", "forbidden"}
                });
                return;
            }
            if (!is_json_content_type(req))
            {
                res.status = 415;
                set_json(res, json{
                                  {"error", "Content-Type must be application/json"}
                });
                return;
            }
            const auto body = json::parse(req.body);
            const auto password = body.at("password").get<std::string>();
            if (password.empty())
                throw std::runtime_error("password is required");
            if (password.size() > 256)
                throw std::runtime_error("password must be 256 characters or fewer");
            store_.update_user_password(user_id, hash_password(password));
            LOGF_INFO("Changed password for user_id=%lld by=%s", static_cast<long long>(user_id),
                      session->username.c_str());
            set_json(res, json{
                              {"ok", true}
            });
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            set_json(res, json{
                              {"error", e.what()}
            });
        }
    });

    // GET /api/silences — list active and future silence rules (operator+)
    http_->Get("/api/silences", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = require_role(req, res, "operator");
        if (!session)
            return;
        auto silences = store_.list_silences();
        json arr = json::array();
        for (const auto& s : silences)
        {
            if (is_global_role(session->role) || (s.agent_id != "*" && can_access_agent(*session, s.agent_id)))
                arr.push_back(silence_to_json(s));
        }
        set_json(res, arr);
    });

    // POST /api/silences — create a silence rule (operator+)
    http_->Post("/api/silences", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = require_role(req, res, "operator");
        if (!session)
            return;
        try
        {
            if (!is_json_content_type(req))
            {
                res.status = 415;
                set_json(res, json{
                                  {"error", "Content-Type must be application/json"}
                });
                return;
            }
            const auto body = json::parse(req.body);
            SilenceRecord rec;
            rec.agent_id = body.value("agent_id", std::string("*"));
            rec.indicator = body.value("indicator", std::string("*"));
            rec.reason = body.value("reason", std::string(""));
            if (!is_global_role(session->role))
            {
                if (rec.agent_id == "*" || !can_access_agent(*session, rec.agent_id))
                {
                    res.status = 403;
                    set_json(res, json{
                                      {"error", "silence is outside the user's groups"}
                    });
                    return;
                }
            }
            if (rec.reason.size() > 1024)
                throw std::runtime_error("reason must be 1024 characters or fewer");
            rec.until_ms = body.at("until_ms").get<int64_t>();
            rec.created_by = session->username;
            rec.created_at = now_ms();
            if (rec.until_ms <= rec.created_at)
                throw std::runtime_error("until_ms must be in the future");
            const auto silence_id = store_.create_silence(rec);
            LOGF_INFO("Created silence silence_id=%lld agent_id=%s indicator=%s by=%s",
                      static_cast<long long>(silence_id), rec.agent_id.c_str(), rec.indicator.c_str(),
                      session->username.c_str());
            set_json(res, json{
                              {"silence_id", silence_id}
            });
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            set_json(res, json{
                              {"error", e.what()}
            });
        }
    });

    // DELETE /api/silences/:id — remove a silence rule (operator+)
    http_->Delete("/api/silences/:id", [this](const httplib::Request& req, httplib::Response& res) {
        auto session = require_role(req, res, "operator");
        if (!session)
            return;
        try
        {
            const auto silence_id = std::stoll(req.path_params.at("id"));
            if (!is_global_role(session->role))
            {
                bool allowed = false;
                for (const auto& silence : store_.list_silences())
                {
                    if (silence.silence_id == silence_id && silence.agent_id != "*" &&
                        can_access_agent(*session, silence.agent_id))
                    {
                        allowed = true;
                        break;
                    }
                }
                if (!allowed)
                {
                    res.status = 403;
                    set_json(res, json{
                                      {"error", "silence is outside the user's groups"}
                    });
                    return;
                }
            }
            store_.delete_silence(silence_id);
            set_json(res, json{
                              {"ok", true}
            });
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            set_json(res, json{
                              {"error", e.what()}
            });
        }
    });

    // GET /api/agents/:id/history?limit=N — status history for one agent (viewer+)
    http_->Get("/api/agents/:id/history", [this](const httplib::Request& req, httplib::Response& res) {
        try
        {
            const auto id = req.path_params.at("id");
            // SEC-11: history carries indicator transitions and alert
            // messages for one agent — enforce the same group scoping as
            // every other per-agent endpoint.
            if (!require_agent_access(req, res, "viewer", id))
                return;
            int limit = 100;
            if (req.has_param("limit"))
                limit = std::min(200, std::max(1, std::stoi(req.get_param_value("limit"))));
            auto rows = store_.list_status_history(id, limit);
            json arr = json::array();
            for (const auto& h : rows)
                arr.push_back(status_history_to_json(h));
            set_json(res, arr);
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            set_json(res, json{
                              {"error", e.what()}
            });
        }
    });

    // ── Views ─────────────────────────────────────────────────────────────────

    auto view_to_json = [](const ViewRecord& v) -> json {
        return {
            {"view_id",        v.view_id       },
            {"name",           v.name          },
            {"owner_user_id",  v.owner_user_id },
            {"owner_username", v.owner_username},
            {"is_public",      v.is_public     },
            {"group_id",       v.group_id      },
            {"agent_ids",      v.agent_ids     },
            {"created_at",     v.created_at    },
        };
    };

    // GET /api/views — list views accessible to current user (own + public)
    http_->Get("/api/views", [this, view_to_json](const httplib::Request& req, httplib::Response& res) {
        try
        {
            auto session = require_role(req, res, "viewer");
            if (!session)
                return;
            auto rows = store_.list_views(is_global_role(session->role) ? 0 : session->user_id);
            json arr = json::array();
            for (const auto& v : rows)
            {
                if (group_visible_to_session(store_, *session, v.group_id))
                    arr.push_back(view_to_json(v));
            }
            set_json(res, arr);
        }
        catch (const std::exception& e)
        {
            LOGF_WARNING("GET /api/views failed: %s", e.what());
            res.status = 500;
            set_json(res, json{
                              {"error", "internal error"}
            });
        }
    });

    // POST /api/views — create a view (operator+)
    http_->Post("/api/views", [this, view_to_json](const httplib::Request& req, httplib::Response& res) {
        try
        {
            auto session = require_role(req, res, "operator");
            if (!session)
                return;
            auto body = json::parse(req.body);
            ViewRecord rec;
            rec.name = body.at("name").get<std::string>();
            if (rec.name.empty())
                throw std::runtime_error("name is required");
            rec.owner_user_id = session->user_id;
            rec.is_public = body.value("is_public", false);
            rec.group_id = body.value("group_id", static_cast<int64_t>(0));
            if (!is_global_role(session->role))
            {
                const auto groups = store_.get_user_groups(session->user_id);
                if (std::find(groups.begin(), groups.end(), rec.group_id) == groups.end())
                    throw std::runtime_error("view group is outside the user's groups");
            }
            if (body.contains("agent_ids") && body["agent_ids"].is_array())
                rec.agent_ids = body["agent_ids"].get<std::vector<std::string>>();
            // SEC-11: a view may only reference agents its creator can see.
            if (!is_global_role(session->role))
            {
                for (const auto& agent_id : rec.agent_ids)
                {
                    if (!can_access_agent(*session, agent_id))
                        throw std::runtime_error("view references an agent outside your access");
                }
            }
            rec.created_at = now_ms();
            const auto id = store_.create_view(rec);
            rec.view_id = id;
            rec.owner_username = session->username;
            res.status = 201;
            set_json(res, view_to_json(rec));
        }
        catch (const std::exception& e)
        {
            LOGF_WARNING("POST /api/views failed: %s", e.what());
            res.status = 400;
            set_json(res, json{
                              {"error", e.what()}
            });
        }
    });

    // GET /api/views/:id
    http_->Get("/api/views/:id", [this, view_to_json](const httplib::Request& req, httplib::Response& res) {
        try
        {
            auto session = require_role(req, res, "viewer");
            if (!session)
                return;
            const auto id = std::stoll(req.path_params.at("id"));
            auto v = store_.get_view(id);
            if (!v)
            {
                res.status = 404;
                set_json(res, json{
                                  {"error", "not found"}
                });
                return;
            }
            if (!group_visible_to_session(store_, *session, v->group_id))
            {
                res.status = 403;
                set_json(res, json{
                                  {"error", "forbidden"}
                });
                return;
            }
            set_json(res, view_to_json(*v));
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            set_json(res, json{
                              {"error", e.what()}
            });
        }
    });

    // PUT /api/views/:id — update (owner or admin)
    http_->Put("/api/views/:id", [this, view_to_json](const httplib::Request& req, httplib::Response& res) {
        try
        {
            auto session = require_role(req, res, "operator");
            if (!session)
                return;
            const auto id = std::stoll(req.path_params.at("id"));
            auto existing = store_.get_view(id);
            if (!existing)
            {
                res.status = 404;
                set_json(res, json{
                                  {"error", "not found"}
                });
                return;
            }
            if (existing->owner_user_id != session->user_id && !is_global_role(session->role))
            {
                res.status = 403;
                set_json(res, json{
                                  {"error", "forbidden"}
                });
                return;
            }
            auto body = json::parse(req.body);
            existing->name = body.value("name", existing->name);
            if (existing->name.empty())
                throw std::runtime_error("name is required");
            existing->is_public = body.value("is_public", existing->is_public);
            existing->group_id = body.value("group_id", existing->group_id);
            if (!is_global_role(session->role))
            {
                const auto groups = store_.get_user_groups(session->user_id);
                if (std::find(groups.begin(), groups.end(), existing->group_id) == groups.end())
                    throw std::runtime_error("view group is outside the user's groups");
            }
            if (body.contains("agent_ids") && body["agent_ids"].is_array())
                existing->agent_ids = body["agent_ids"].get<std::vector<std::string>>();
            // SEC-11: a view may only reference agents its editor can see.
            if (!is_global_role(session->role))
            {
                for (const auto& agent_id : existing->agent_ids)
                {
                    if (!can_access_agent(*session, agent_id))
                        throw std::runtime_error("view references an agent outside your access");
                }
            }
            store_.update_view(*existing);
            set_json(res, view_to_json(*existing));
        }
        catch (const std::exception& e)
        {
            LOGF_WARNING("PUT /api/views/:id failed: %s", e.what());
            res.status = 400;
            set_json(res, json{
                              {"error", e.what()}
            });
        }
    });

    // DELETE /api/views/:id — owner or admin
    http_->Delete("/api/views/:id", [this](const httplib::Request& req, httplib::Response& res) {
        try
        {
            auto session = require_role(req, res, "operator");
            if (!session)
                return;
            const auto id = std::stoll(req.path_params.at("id"));
            auto existing = store_.get_view(id);
            if (!existing)
            {
                res.status = 404;
                set_json(res, json{
                                  {"error", "not found"}
                });
                return;
            }
            if (existing->owner_user_id != session->user_id && !is_global_role(session->role))
            {
                res.status = 403;
                set_json(res, json{
                                  {"error", "forbidden"}
                });
                return;
            }
            store_.delete_view(id);
            set_json(res, json{
                              {"ok", true}
            });
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            set_json(res, json{
                              {"error", e.what()}
            });
        }
    });

    // ── Runbooks ──────────────────────────────────────────────────────────────

    // GET /api/runbooks — list runbooks visible to the session (viewer+)
    http_->Get("/api/runbooks", [this](const httplib::Request& req, httplib::Response& res) {
        try
        {
            auto session = require_role(req, res, "viewer");
            if (!session)
                return;
            auto rows = store_.list_runbooks();
            json arr = json::array();
            for (const auto& r : rows)
            {
                // SEC-11: generic runbooks (no agent_id) are shared knowledge;
                // agent-specific ones are only visible to sessions that can
                // already see that agent.
                if (r.agent_id.empty() || is_global_role(session->role) || can_access_agent(*session, r.agent_id))
                    arr.push_back(runbook_to_json(r));
            }
            set_json(res, arr);
        }
        catch (const std::exception& e)
        {
            res.status = 500;
            set_json(res, json{
                              {"error", e.what()}
            });
        }
    });

    // POST /api/runbooks — create a runbook (admin only)
    http_->Post("/api/runbooks", [this](const httplib::Request& req, httplib::Response& res) {
        try
        {
            auto session = require_role(req, res, "viewer");
            if (!session)
                return;
            if (normalize_role(session->role) != "global_admin")
            {
                res.status = 403;
                set_json(res, json{
                                  {"error", "permission denied"}
                });
                return;
            }
            if (!is_json_content_type(req))
            {
                res.status = 415;
                set_json(res, json{
                                  {"error", "Content-Type must be application/json"}
                });
                return;
            }
            const auto body = json::parse(req.body);
            RunbookRecord rec;
            // SEC-11: an optional agent_id scopes the runbook to one agent;
            // it is only shown to sessions that can see that agent.
            rec.agent_id = body.value("agent_id", std::string(""));
            rec.indicator = body.value("indicator", std::string("*"));
            rec.status = body.at("status").get<std::string>();
            rec.url = body.at("url").get<std::string>();
            rec.notes = body.value("notes", std::string(""));
            rec.created_by = session->username;
            rec.created_at = now_ms();
            if (rec.url.empty())
                throw std::runtime_error("url is required");
            if (rec.url.rfind("https://", 0) != 0 && rec.url.rfind("http://", 0) != 0)
                throw std::runtime_error("url must start with https:// or http://");
            if (rec.status != "yellow" && rec.status != "amber" && rec.status != "red")
                throw std::runtime_error("status must be yellow, amber, or red");
            const auto id = store_.create_runbook(rec);
            rec.runbook_id = id;
            res.status = 201;
            set_json(res, runbook_to_json(rec));
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            set_json(res, json{
                              {"error", e.what()}
            });
        }
    });

    // DELETE /api/runbooks/:id — delete a runbook (admin only)
    http_->Delete("/api/runbooks/:id", [this](const httplib::Request& req, httplib::Response& res) {
        try
        {
            auto session = require_role(req, res, "viewer");
            if (!session)
                return;
            if (normalize_role(session->role) != "global_admin")
            {
                res.status = 403;
                set_json(res, json{
                                  {"error", "permission denied"}
                });
                return;
            }
            const auto id = std::stoll(req.path_params.at("id"));
            store_.delete_runbook(id);
            set_json(res, json{
                              {"ok", true}
            });
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            set_json(res, json{
                              {"error", e.what()}
            });
        }
    });
}

} // namespace thewatcher::server
