#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
using std::string;
constexpr size_t MAX_LINE = 65536;
constexpr size_t MAX_RESPONSE = 32768;

// Output is always JSON, including errors. No dependency on a JSON runtime.
string json(const string &s) {
    string out = "\"";
    const char *hex = "0123456789abcdef";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 32) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else
            out += static_cast<char>(c);
    }
    return out + '"';
}
string lower(string s) {
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool contains_any(const string &s, std::initializer_list<const char *> words) {
    for (const auto *word : words)
        if (s.find(word) != string::npos)
            return true;
    return false;
}
// Replace invalid UTF-8 so binary and damaged logs still produce valid JSON.
string utf8(const string &s) {
    string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        size_t n =
            c < 128 ? 1 : (c >= 194 && c <= 223 ? 2 : (c <= 239 && c >= 224 ? 3 : (c >= 240 && c <= 244 ? 4 : 0)));
        bool valid = n && i + n <= s.size();
        for (size_t j = 1; valid && j < n; ++j)
            valid = (static_cast<unsigned char>(s[i + j]) & 0xc0) == 0x80;
        if (valid && n >= 3) {
            unsigned char b = s[i + 1];
            valid =
                !(c == 224 && b < 160) && !(c == 237 && b >= 160) && !(c == 240 && b < 144) && !(c == 244 && b >= 144);
        }
        if (valid) {
            out.append(s, i, n);
            i += n;
        } else {
            out += "\xef\xbf\xbd";
            ++i;
        }
    }
    return out;
}
string clip(const string &s, size_t n) {
    if (s.size() <= n)
        return s;
    size_t end = n;
    while (end && (static_cast<unsigned char>(s[end]) & 0xc0) == 0x80)
        --end;
    return s.substr(0, end) + "...";
}

struct Statement {
    sqlite3_stmt *stmt = nullptr;
    explicit Statement(sqlite3 *db, const string &sql) {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(stmt); }
    Statement(const Statement &) = delete;
    void bind(int i, const string &s) {
        if (sqlite3_bind_text(stmt, i, s.c_str(), static_cast<int>(s.size()), SQLITE_TRANSIENT) != SQLITE_OK)
            throw std::runtime_error("SQLite bind failed");
    }
    void bind(int i, long long n) {
        if (sqlite3_bind_int64(stmt, i, n) != SQLITE_OK)
            throw std::runtime_error("SQLite bind failed");
    }
    bool step() {
        int rc = sqlite3_step(stmt);
        if (rc != SQLITE_ROW && rc != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(stmt)));
        return rc == SQLITE_ROW;
    }
    void reset() {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }
    string str(int i) const {
        const auto *p = sqlite3_column_text(stmt, i);
        return p ? reinterpret_cast<const char *>(p) : "";
    }
    long long num(int i) const { return sqlite3_column_int64(stmt, i); }
};
struct Database {
    sqlite3 *db = nullptr;
    explicit Database(const string &filename) {
        fs::path p = fs::u8path(filename);
        if (p.has_parent_path())
            fs::create_directories(p.parent_path());
        if (sqlite3_open_v2(filename.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
            string error = db ? sqlite3_errmsg(db) : "Cannot open database";
            if (db)
                sqlite3_close(db);
            db = nullptr;
            throw std::runtime_error(error);
        }
        sqlite3_busy_timeout(db, 30000);
        try {
            exec("PRAGMA foreign_keys=ON;");
            // Query an existing WAL database without acquiring a write lock.
            string journal;
            {
                Statement mode(db, "PRAGMA journal_mode");
                mode.step();
                journal = mode.str(0);
            }
            if (journal != "wal")
                exec("PRAGMA journal_mode=WAL");
            long long schema_version = 0;
            {
                Statement version(db, "PRAGMA user_version");
                version.step();
                schema_version = version.num(0);
            }
            if (schema_version > 1)
                throw std::runtime_error("Database schema is newer than this program");
            if (!schema_version) {
                exec("BEGIN IMMEDIATE");
                exec("CREATE TABLE IF NOT EXISTS sources(id INTEGER PRIMARY KEY, name TEXT NOT NULL, format TEXT NOT "
                     "NULL, created TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')), lines INTEGER NOT "
                     "NULL DEFAULT 0, bytes INTEGER NOT NULL DEFAULT 0, truncated_lines INTEGER NOT NULL DEFAULT 0, "
                     "redacted_lines INTEGER NOT NULL DEFAULT 0, exit_code INTEGER, success INTEGER NOT NULL DEFAULT "
                     "0, redaction INTEGER NOT NULL);"
                     "CREATE TABLE IF NOT EXISTS lines(source_id INTEGER NOT NULL REFERENCES sources(id), number "
                     "INTEGER NOT NULL, text TEXT NOT NULL, PRIMARY KEY(source_id,number));"
                     "CREATE TABLE IF NOT EXISTS issues(id INTEGER PRIMARY KEY, source_id INTEGER NOT NULL REFERENCES "
                     "sources(id), severity TEXT NOT NULL, rule TEXT NOT NULL, message TEXT NOT NULL, fingerprint TEXT "
                     "NOT NULL, first_line INTEGER NOT NULL, last_line INTEGER NOT NULL, count INTEGER NOT NULL "
                     "DEFAULT 1, UNIQUE(source_id,severity,rule,fingerprint));"
                     "CREATE INDEX IF NOT EXISTS issues_source ON issues(source_id,severity,first_line); PRAGMA "
                     "user_version=1;");
                // FTS5 is optional; literal substring search is available on every build.
                char *error = nullptr;
                int rc = sqlite3_exec(db,
                                      "CREATE VIRTUAL TABLE IF NOT EXISTS line_search USING fts5(text, "
                                      "content='lines', content_rowid='rowid');",
                                      nullptr, nullptr, &error);
                sqlite3_free(error);
                fts = rc == SQLITE_OK;
                if (fts)
                    exec("INSERT INTO line_search(line_search) VALUES('rebuild')");
                exec("COMMIT");
            }
            Statement exists(db, "SELECT count(*) FROM sqlite_master WHERE name='line_search'");
            exists.step();
            fts = exists.num(0) != 0;
        } catch (...) {
            sqlite3_close(db);
            db = nullptr;
            throw;
        }
    }
    bool fts = false;
    ~Database() {
        if (db)
            sqlite3_close(db);
    }
    void exec(const string &sql) {
        char *error = nullptr;
        if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
            string msg = error ? error : "SQLite error";
            sqlite3_free(error);
            throw std::runtime_error(msg);
        }
    }
};

struct Rule {
    string id, severity;
    std::regex pattern;
};
std::regex rx(const string &s) {
    try {
        return std::regex(s, std::regex::icase | std::regex::optimize);
    } catch (const std::regex_error &e) {
        throw std::runtime_error("Invalid built-in pattern: " + s + " (" + e.what() + ")");
    }
}
bool matches(const string &s, const std::regex &pattern, size_t *position = nullptr) {
    // libstdc++ uses recursive regex evaluation. Never hand it an unbounded
    // quantifier over a whole 64 KiB line: that can exhaust the Linux stack.
    constexpr size_t window = 1024, overlap = 256;
    for (size_t start = 0;;) {
        const auto end = std::min(s.size(), start + window);
        auto flags = std::regex_constants::match_default;
        if (start)
            flags |= std::regex_constants::match_not_bol | std::regex_constants::match_prev_avail;
        if (end < s.size())
            flags |= std::regex_constants::match_not_eol;
        std::match_results<string::const_iterator> result;
        if (std::regex_search(s.cbegin() + start, s.cbegin() + end, result, pattern, flags)) {
            if (position)
                *position = start + static_cast<size_t>(result.position());
            return true;
        }
        if (end == s.size())
            return false;
        start = end - overlap;
    }
}
struct Envelope {
    string kind, priority, tag, pid, tid, message;
    bool error() const {
        const auto level = lower(priority);
        return level == "e" || level == "f" || level == "a" || level == "error" || level == "fatal" ||
               level == "critical";
    }
    bool warning() const { return lower(priority) == "warn" || lower(priority) == "warning" || lower(priority) == "w"; }
    bool continuation() const {
        const auto body = lower(message);
        return body.rfind("at ", 0) == 0 || body.rfind("... ", 0) == 0 || body.rfind("process:", 0) == 0 ||
               body.rfind("pid:", 0) == 0 ||
               (body.size() > 1 && body[0] == '#' && std::isdigit(static_cast<unsigned char>(body[1])));
    }
};
Envelope envelope(const string &text) {
    if (text.empty() ||
        (!std::isdigit(static_cast<unsigned char>(text[0])) && string("VDIWEFAvdiwefa").find(text[0]) == string::npos))
        return {};
    struct Layout {
        const char *kind;
        std::regex header;
        int priority, tag, pid, tid;
    };
    static const std::vector<Layout> layouts = {
        {"logcat",
         rx("^(?:[0-9]{4}-)?[0-9]{2}-[0-9]{2}[[:space:]]+[0-9]{2}:[0-9]{2}:[0-9]{2}[.,][0-9]+[[:space:]]+([0-9]+)[[:"
            "space:]]+([0-9]+)[[:space:]]+([VDIWEFA])[[:space:]]+([^:]+):[[:space:]]*"),
         3, 4, 1, 2},
        {"logcat",
         rx("^(?:(?:[0-9]{4}-)?[0-9]{2}-[0-9]{2}[[:space:]]+[0-9]{2}:[0-9]{2}:[0-9]{2}[.,][0-9]+[[:space:]]+)?(["
            "VDIWEFA])/([^(:]+)\\([[:space:]]*([0-9]+)\\)[[:space:]]*:[[:space:]]*"),
         1, 2, 3, 0},
        {"logcat", rx("^([VDIWEFA])/([^:]+):[[:space:]]*"), 1, 2, 0, 0},
        {"logcat",
         rx("^[0-9]{4}-[0-9]{2}-[0-9]{2}[[:space:]]+[0-9]{2}:[0-9]{2}:[0-9]{2}[.,][0-9]+[[:space:]]+([0-9]+)-([0-9]+)[["
            ":space:]]+([^[:space:]]+)[[:space:]]+(?:[^[:space:]]+[[:space:]]+)?([VDIWEFA])[[:space:]]+"),
         4, 3, 1, 2},
        {"jetbrains",
         rx("^[0-9]{4}-[0-9]{2}-[0-9]{2}[ "
            "T][0-9]{2}:[0-9]{2}:[0-9]{2}[.,][0-9]+[[:space:]]+\\[[[:space:]]*[0-9]+\\][[:space:]]+(TRACE|DEBUG|INFO|"
            "WARN|ERROR|FATAL)[[:space:]]+-[[:space:]]+([^[:space:]]+)[[:space:]]+-[[:space:]]*"),
         1, 2, 0, 0},
        {"vscode",
         rx("^[0-9]{4}-[0-9]{2}-[0-9]{2}[ "
            "T][0-9]{2}:[0-9]{2}:[0-9]{2}[.,][0-9]+[[:space:]]+\\[(trace|debug|info|warn|warning|error|critical)\\][[:"
            "space:]]*(?:\\[([^\\]]+)\\][[:space:]]*)?"),
         1, 2, 0, 0}};
    const auto prefix = text.substr(0, 512);
    for (const auto &layout : layouts) {
        std::smatch match;
        if (!std::regex_search(prefix, match, layout.header))
            continue;
        Envelope out;
        out.kind = layout.kind;
        out.priority = match[layout.priority].str();
        if (layout.tag)
            out.tag = match[layout.tag].str();
        while (!out.tag.empty() && std::isspace(static_cast<unsigned char>(out.tag.back())))
            out.tag.pop_back();
        if (layout.pid)
            out.pid = match[layout.pid].str();
        if (layout.tid)
            out.tid = match[layout.tid].str();
        out.message = text.substr(static_cast<size_t>(match.length()));
        return out;
    }
    return {};
}
const std::vector<Rule> &rules() {
    static const std::vector<Rule> items = {
        {"github.error", "error", rx("::error(?:[ :]|::)|##\\[error\\]")},
        {"github.warning", "warning", rx("::warning(?:[ :]|::)|##\\[warning\\]")},
        {"exception.cause", "error", rx("(?:^|[[:space:]])Caused by:[[:space:]]*.+")},
        {"android.crash", "error", rx("\\bFATAL EXCEPTION:|\\bFatal signal [0-9]+")},
        {"xcode.signing", "error",
         rx("Signing for .+ requires a development team|No profiles for .+ were found|Command CodeSign "
            "failed|Provisioning profile .+doesn't include")},
        {"vscode.extension", "error",
         rx("Activating extension .+ failed|Extension host .*(?:crashed|terminated unexpectedly|exited with code "
            "[1-9])")},
        {"gradle.failed", "error",
         rx("FAILURE: Build failed with an exception|Execution failed for task|Could not resolve all "
            "(?:files|dependencies)")},
        {"make.target", "error",
         rx("No rule to make target|[Mm]akefile:[0-9]+:.*(?:missing separator|recipe commences before first target)")},
        {"cmake.error", "error", rx("CMake Error(?: at| in|:)")},
        {"compiler.kotlin", "error", rx("(?:^|[[:space:]])e:[[:space:]]+(?:file://|[^[:space:]]+\\.kt:)")},
        {"dependency.missing", "error",
         rx("fatal error:.*(?:No such file or directory|file not found)|Cannot open include file:|"
            "ModuleNotFoundError:|No module named |Could NOT find [A-Za-z0-9_]+")},
        {"crash.signal", "error",
         rx("segmentation fault|segfault|bus "
            "error|EXC_BAD_ACCESS|EXC_CRASH|SIGSEGV|SIGABRT|SIGBUS|SIGILL|SIGFPE|addresssanitizer|"
            "undefinedbehaviorsanitizer|threadsanitizer|runtime error:")},
        {"crash.apple", "error",
         rx("Exception Type:|Termination Reason:|Crashed Thread:|Thread [0-9]+ "
            "Crashed:|\"(?:exception|termination)\"[[:space:]]*:[[:space:]]*\\{")},
        {"exception.unhandled", "error",
         rx("Traceback \\(most recent call last\\)|unhandled (?:exception|rejection)|uncaught "
            "(?:exception|error)|panic:|thread .+ panicked|Exception in thread")},
        {"compiler.error", "error",
         rx("(?:^|[ :])(?:fatal error|error)[[:space:]]*:[[:space:]]*.+|\\berror [A-Z]+[0-9]+:")},
        {"linker.error", "error",
         rx("undefined (?:reference|symbols)|unresolved external|duplicate symbol|ld: .*error|linker command "
            "failed|cannot find -l")},
        {"build.failed", "error",
         rx("\\*\\* (?:BUILD|TEST|ARCHIVE|EXPORT) FAILED \\*\\*|BUILD FAILED|FAILED:|ninja: build "
            "stopped|make(?:\\[[0-9]+\\])?: \\*\\*\\*|MSBUILD : error|CMake Error|Process completed with exit code "
            "[1-9]|exited with (?:code|status) [1-9]")},
        {"test.failed", "error",
         rx("(?:^|[[:space:]])FAIL(?:ED)?[[:space:]]*:|"
            "(?:^|[[:space:]])FAIL(?:ED)?[[:space:]]+(?:test[_s/.][^[:space:]]*|\\((?:failures|errors)=)|"
            "[1-9][0-9]* (?:tests? )?(?:failed|failures|errors)(?:[, .]|$)|test case .+ "
            "failed|The following tests FAILED|^FAIL(?:ED)?$")},
        {"log.error", "error",
         rx("(?:^|[[:space:]:\\]])(?:ERROR|FATAL|CRITICAL)\\b[[:space:]]*[:\\]]|\\[(?:ERROR|FATAL|CRITICAL)\\]|\"(?:"
            "level|severity|"
            "levelname)\"[[:space:]]*:[[:space:]]*\"(?:error|fatal|critical)\"|\\b[a-zA-Z_][a-zA-Z0-9_.]*(?:Error|"
            "Exception):")},
        {"compiler.warning", "warning", rx("\\bwarning[ :]+|\\bwarning [A-Z]+[0-9]+:")},
        {"log.warning", "warning",
         rx("\\[(?:WARN|WARNING)\\]|\\bWARN[[:space:]]*[: "
            "]|\"(?:level|severity|levelname)\"[[:space:]]*:[[:space:]]*\"(?:warn|warning)\"")}};
    return items;
}
string sanitize(const string &s) {
    // Strip terminal escape sequences without executing them.
    string out;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = s[i];
        if (c == 27) {
            if (i + 1 < s.size() && s[i + 1] == '[') {
                i += 2;
                while (i < s.size() && !(s[i] >= '@' && s[i] <= '~'))
                    ++i;
            } else if (i + 1 < s.size() && s[i + 1] == ']') {
                i += 2;
                while (i < s.size() && s[i] != '\a' && !(s[i] == 27 && i + 1 < s.size() && s[i + 1] == '\\'))
                    ++i;
                if (i < s.size() && s[i] == 27)
                    ++i;
            } else if (i + 1 < s.size())
                ++i;
        } else if ((c >= 32 && c != 127) || c == '\t')
            out += static_cast<char>(c);
    }
    return utf8(out);
}
struct Redactor {
    bool private_key = false;
    string apply(string s) {
        if (s.find("-----BEGIN ") != string::npos && s.find("PRIVATE KEY-----") != string::npos)
            private_key = true;
        if (private_key) {
            if (s.find("-----END ") != string::npos && s.find("PRIVATE KEY-----") != string::npos)
                private_key = false;
            return "[REDACTED PRIVATE KEY]";
        }
        if (!contains_any(lower(s),
                          {"authorization", "bearer",  "password", "passwd",     "pwd",         "token",     "secret",
                           "api_key",       "api-key", "apikey",   "access_key", "access-key",  "accesskey", "ghp_",
                           "gho_",          "ghu_",    "ghs_",     "ghr_",       "github_pat_", "akia",      "://"}))
            return s;
        const auto text = lower(s);
        std::vector<std::pair<size_t, size_t>> spans;
        auto space = [&](size_t i) { return i < s.size() && std::isspace(static_cast<unsigned char>(s[i])); };
        auto value_end = [&](size_t start, char quote) {
            size_t end = start;
            while (end < s.size()) {
                if (quote) {
                    if (s[end] == '\\' && end + 1 < s.size()) {
                        end += 2;
                        continue;
                    }
                    if (s[end] == quote)
                        break;
                } else if (space(end) || s[end] == '&' || s[end] == ',' || s[end] == ';' || s[end] == '"' ||
                           s[end] == '\'')
                    break;
                ++end;
            }
            return end;
        };
        for (const string key :
             {"authorization", "password", "passwd", "pwd", "token", "secret", "api_key", "api-key", "apikey",
              "access_key", "access-key", "accesskey", "client_secret", "client-secret", "clientsecret"}) {
            size_t position = 0;
            while ((position = text.find(key, position)) != string::npos) {
                size_t i = position + key.size();
                position = i;
                if (i < s.size() && (s[i] == '"' || s[i] == '\''))
                    ++i;
                while (space(i))
                    ++i;
                if (i == s.size() || (s[i] != ':' && s[i] != '='))
                    continue;
                ++i;
                while (space(i))
                    ++i;
                char quote = 0;
                if (i < s.size() && (s[i] == '"' || s[i] == '\''))
                    quote = s[i++];
                if (key == "authorization") {
                    if (text.compare(i, 7, "bearer ") == 0)
                        i += 7;
                    else if (text.compare(i, 6, "basic ") == 0)
                        i += 6;
                    while (space(i))
                        ++i;
                }
                const auto end = value_end(i, quote);
                if (end > i)
                    spans.emplace_back(i, end);
            }
        }
        // Standalone bearer values and provider tokens may occur without an assignment.
        size_t position = 0;
        while ((position = text.find("bearer", position)) != string::npos) {
            size_t i = position + 6;
            position = i;
            if (!space(i))
                continue;
            while (space(i))
                ++i;
            auto end = value_end(i, 0);
            if (end > i)
                spans.emplace_back(i, end);
        }
        for (const string prefix : {"ghp_", "gho_", "ghu_", "ghs_", "ghr_", "github_pat_", "akia"}) {
            position = 0;
            while ((position = text.find(prefix, position)) != string::npos) {
                size_t end = position + prefix.size();
                while (end < s.size() && (std::isalnum(static_cast<unsigned char>(s[end])) || s[end] == '_'))
                    ++end;
                if (end - position - prefix.size() >= 16)
                    spans.emplace_back(position, end);
                position = end;
            }
        }
        for (const string scheme : {"http://", "https://"}) {
            position = 0;
            while ((position = text.find(scheme, position)) != string::npos) {
                const auto start = position + scheme.size();
                auto end = start;
                while (end < s.size() && s[end] != '/' && !space(end) && s[end] != '"' && s[end] != '\'')
                    ++end;
                const auto at = s.find('@', start), colon = s.find(':', start);
                if (at < end && colon < at)
                    spans.emplace_back(start, at);
                position = end;
            }
        }
        std::sort(spans.begin(), spans.end());
        string out;
        size_t copied = 0;
        for (size_t i = 0; i < spans.size(); ++i) {
            auto start = spans[i].first, end = spans[i].second;
            while (i + 1 < spans.size() && spans[i + 1].first <= end)
                end = std::max(end, spans[++i].second);
            out.append(s, copied, start - copied);
            out += "[REDACTED]";
            copied = end;
        }
        out.append(s, copied, string::npos);
        return out;
    }
};
string fingerprint(string s) {
    static const auto timestamp = rx("^[0-9]{4}-[0-9]{2}-[0-9]{2}[T ][0-9:.+Z-]+[[:space:]]*");
    const auto prefix = s.substr(0, 128);
    s = std::regex_replace(prefix, timestamp, "") + s.substr(prefix.size());
    string out;
    bool previous_space = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const bool space = std::isspace(static_cast<unsigned char>(s[i])) != 0;
        if (space) {
            if (!previous_space)
                out += ' ';
        } else if (s[i] == '0' && i + 2 < s.size() && (s[i + 1] == 'x' || s[i + 1] == 'X') &&
                   std::isxdigit(static_cast<unsigned char>(s[i + 2])) &&
                   (i == 0 || (!std::isalnum(static_cast<unsigned char>(s[i - 1])) && s[i - 1] != '_'))) {
            out += "<address>";
            i += 2;
            while (i + 1 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])))
                ++i;
        } else
            out += s[i];
        previous_space = space;
    }
    return out;
}
bool success_line(const string &s) {
    if (!contains_any(lower(s), {"succeeded", "success", "passed", "ok", "exit code 0", "tests run:"}))
        return false;
    static const auto pattern =
        rx("\\*\\* (?:BUILD|TEST|ARCHIVE|EXPORT) SUCCEEDED \\*\\*|BUILD SUCCESSFUL|BUILD SUCCESS(?:[[:space:]]|$)|100% "
           "tests passed|^OK(?:[[:space:]]*\\([^)]*\\))?[[:space:]]*$|Process completed with exit code "
           "0(?:[^0-9]|$)|^Tests run: [0-9]+, "
           "Failures: 0, Errors: 0|^[0-9]+ passed(?: in |$)");
    return matches(s, pattern);
}
string detect_format(const string &s, string current, const Envelope &entry) {
    if (current != "generic")
        return current;
    if (s.find("::error") != string::npos || s.find("##[group]") != string::npos ||
        s.find("##[error]") != string::npos || s.find("::group::") != string::npos)
        return "github";
    if (s.find("xcodebuild") != string::npos || s.find("** BUILD ") != string::npos ||
        s.find("CompileSwift") != string::npos)
        return "xcode";
    if (s.find("Exception Type:") != string::npos || s.find("Crashed Thread:") != string::npos ||
        s.find("\"incident_id\"") != string::npos || s.find("\"incident\"") != string::npos)
        return "crash";
    if (s.find("Traceback (most recent call last)") != string::npos)
        return "python";
    if (s.find("Android Studio") != string::npos)
        return "android-studio";
    if (!entry.kind.empty())
        return entry.kind;
    if (s.find("CMake") != string::npos || s.rfind("-- Configuring", 0) == 0 || s.rfind("-- Generating", 0) == 0)
        return "cmake";
    if (s.rfind("make:", 0) == 0 || s.rfind("make[", 0) == 0 || s.rfind("gmake", 0) == 0 ||
        s.rfind("Makefile:", 0) == 0)
        return "make";
    if (s.find("Gradle") != string::npos || s.rfind("> Task ", 0) == 0)
        return "gradle";
    if (s.find("ninja:") != string::npos)
        return "build";
    return current;
}

struct Args {
    string command, db, file, name, format = "auto", text, exit_file, output = "json";
    long long source = 0, limit = 10, offset = 0, line = 1, before = 3, after = 3;
    std::optional<long long> exit_code;
    bool redact = true, fts = false, human = false;
    string severity;
};
long long integer(const string &s, long long min, long long max) {
    size_t consumed = 0;
    long long n = std::stoll(s, &consumed);
    if (consumed != s.size() || n < min || n > max)
        throw std::runtime_error("Integer out of range: " + s);
    return n;
}
void parse(Args &a, int argc, char **argv) {
    const char *env = std::getenv("LOGSONFIRE_DB");
    a.db = env ? env : ".logsonfire/logs.db";
    for (int i = 1; i < argc; ++i) {
        string s = argv[i];
        auto value = [&]() -> string {
            if (++i >= argc)
                throw std::runtime_error("Missing value for " + s);
            return argv[i];
        };
        if (s == "--db")
            a.db = value();
        else if (s == "--source")
            a.source = integer(value(), 1, 9223372036854775807LL);
        else if (s == "--limit")
            a.limit = integer(value(), 1, 100);
        else if (s == "--offset")
            a.offset = integer(value(), 0, 2147483647);
        else if (s == "--line")
            a.line = integer(value(), 1, 2147483647);
        else if (s == "--before")
            a.before = integer(value(), 0, 49);
        else if (s == "--after")
            a.after = integer(value(), 0, 49);
        else if (s == "--name")
            a.name = value();
        else if (s == "--format")
            a.format = value();
        else if (s == "--output")
            a.output = value();
        else if (s == "--text")
            a.text = value();
        else if (s == "--severity")
            a.severity = value();
        else if (s == "--exit-code")
            a.exit_code = integer(value(), 0, 4294967295LL);
        else if (s == "--exit-code-file")
            a.exit_file = value();
        else if (s == "--no-redact")
            a.redact = false;
        else if (s == "--fts")
            a.fts = true;
        else if (s == "--human")
            a.human = true;
        else if (s == "--help" || s == "-h")
            a.command = "help";
        else if (s == "--version")
            a.command = "version";
        else if (a.command.empty())
            a.command = s;
        else if ((a.command == "ingest" || a.command == "analyze") && a.file.empty() &&
                 (s == "-" || (!s.empty() && s[0] != '-')))
            a.file = s;
        else
            throw std::runtime_error("Unknown argument: " + s);
    }
    if (a.command.empty())
        a.command = "help";
    if (a.format != "auto" && a.format != "generic" && a.format != "github" && a.format != "xcode" &&
        a.format != "crash" && a.format != "python" && a.format != "build" && a.format != "logcat" &&
        a.format != "android-studio" && a.format != "jetbrains" && a.format != "vscode" && a.format != "cmake" &&
        a.format != "make" && a.format != "gradle")
        throw std::runtime_error("Unknown format");
    if (!a.severity.empty() && a.severity != "error" && a.severity != "warning")
        throw std::runtime_error("Severity must be error or warning");
    if (a.text.size() > 1024 || a.name.size() > 512)
        throw std::runtime_error("Text/name exceeds input limit");
    if (a.human && a.command != "analyze" && a.command != "explain")
        throw std::runtime_error("--human is available for analyze and explain");
    if (a.output != "json" && a.output != "text")
        throw std::runtime_error("Output must be json or text");
}
long long source_id(Database &db, long long id) {
    Statement q(db.db, id ? "SELECT id FROM sources WHERE id=?" : "SELECT id FROM sources ORDER BY id DESC LIMIT 1");
    if (id)
        q.bind(1, id);
    if (!q.step())
        throw std::runtime_error(id ? "Source not found" : "No sources; ingest a log first");
    return q.num(0);
}
// Bound physical lines without ever allocating an entire pathological line.
bool read_line(std::istream &input, string &out, long long &bytes, bool &truncated) {
    out.clear();
    truncated = false;
    auto *buf = input.rdbuf();
    bool any = false;
    for (;;) {
        auto c = buf->sbumpc();
        if (c == std::char_traits<char>::eof())
            return any;
        any = true;
        ++bytes;
        if (c == '\n')
            return true;
        if (out.size() < MAX_LINE)
            out += static_cast<char>(c);
        else
            truncated = true;
    }
}
long long ingest(Database &db, Args &a) {
    if (a.file.empty())
        throw std::runtime_error("ingest requires a file or - for stdin");
    std::ifstream file;
    std::istream *input = &std::cin;
    if (a.file != "-") {
        file.open(fs::u8path(a.file), std::ios::binary);
        if (!file)
            throw std::runtime_error("Cannot read log: " + a.file);
        input = &file;
    }
    string name = a.name.empty() ? (a.file == "-" ? "stdin" : fs::u8path(a.file).filename().u8string()) : a.name;
    Redactor redactor;
    name = sanitize(name);
    if (a.redact)
        name = redactor.apply(name);
    redactor.private_key = false;
    string format = a.format == "auto" ? "generic" : a.format;
    if (a.format == "auto" && a.file != "-") {
        const auto basename = lower(fs::u8path(a.file).filename().u8string());
        if (basename == "idea.log")
            format = "jetbrains";
        if (basename == "exthost.log")
            format = "vscode";
    }
    db.exec("BEGIN IMMEDIATE");
    try {
        Statement add(db.db, "INSERT INTO sources(name,format,redaction) VALUES(?,?,?)");
        add.bind(1, name);
        add.bind(2, format);
        add.bind(3, a.redact ? 1LL : 0LL);
        add.step();
        long long id = sqlite3_last_insert_rowid(db.db);
        Statement line(db.db, "INSERT INTO lines(source_id,number,text) VALUES(?,?,?)");
        Statement issue(db.db, "INSERT INTO issues(source_id,severity,rule,message,fingerprint,first_line,last_line) "
                               "VALUES(?,?,?,?,?,?,?) ON CONFLICT(source_id,severity,rule,fingerprint) DO UPDATE SET "
                               "count=count+1,last_line=excluded.last_line");
        std::optional<Statement> index;
        if (db.fts)
            index.emplace(db.db, "INSERT INTO line_search(rowid,text) VALUES(?,?)");
        string raw;
        long long total = 0, bytes = 0, truncated_count = 0, redacted_count = 0;
        bool success = false, truncated = false;
        int cmake_error_context = 0;
        while (read_line(*input, raw, bytes, truncated)) {
            ++total;
            if (truncated)
                ++truncated_count;
            string text = sanitize(raw);
            if (a.redact) {
                string clean = redactor.apply(text);
                if (clean != text)
                    ++redacted_count;
                text = std::move(clean);
            }
            const auto entry = envelope(text);
            cmake_error_context = text.find("CMake Error") != string::npos ? 16 : std::max(0, cmake_error_context - 1);
            if (a.format == "auto")
                format = detect_format(text, format, entry);
            if (entry.kind.empty())
                success = success || success_line(text);
            line.bind(1, id);
            line.bind(2, total);
            line.bind(3, text);
            line.step();
            line.reset();
            if (index) {
                index->bind(1, sqlite3_last_insert_rowid(db.db));
                index->bind(2, text);
                index->step();
                index->reset();
            }
            if (!entry.kind.empty() && ((!entry.error() && !entry.warning()) || entry.continuation()))
                continue;
            auto save_issue = [&](const string &severity, const string &rule, size_t position) {
                issue.bind(1, id);
                issue.bind(2, severity);
                issue.bind(3, rule);
                size_t excerpt_start = position > 300 ? position - 100 : 0;
                while (excerpt_start && (static_cast<unsigned char>(text[excerpt_start]) & 0xc0) == 0x80)
                    --excerpt_start;
                const auto excerpt = excerpt_start ? "..." + text.substr(excerpt_start) : text;
                issue.bind(4, clip(excerpt, 2000));
                issue.bind(5, entry.kind.empty() ? fingerprint(text)
                                                 : entry.kind + "|" + entry.pid + "|" + entry.tid + "|" + entry.tag +
                                                       "|" + fingerprint(entry.message));
                issue.bind(6, total);
                issue.bind(7, total);
                issue.step();
                issue.reset();
            };
            if (entry.warning()) {
                save_issue(entry.warning() ? "warning" : "error", entry.kind + ".warning", 0);
                continue;
            }
            if (entry.kind.empty() && !contains_any(lower(text), {"error",
                                                                  "fatal",
                                                                  "critical",
                                                                  "warn",
                                                                  "fail",
                                                                  "exception",
                                                                  "crash",
                                                                  "segmentation",
                                                                  "segfault",
                                                                  "bus error",
                                                                  "sigsegv",
                                                                  "sigabrt",
                                                                  "sigbus",
                                                                  "sigill",
                                                                  "sigfpe",
                                                                  "sanitizer",
                                                                  "traceback",
                                                                  "panic",
                                                                  "undefined",
                                                                  "unresolved",
                                                                  "duplicate symbol",
                                                                  "cannot find -l",
                                                                  "build stopped",
                                                                  "termination",
                                                                  "could not find",
                                                                  "no module named",
                                                                  "caused by",
                                                                  "no rule to make target",
                                                                  "missing separator",
                                                                  "signing for",
                                                                  "no profiles for",
                                                                  "e: "}))
                continue;
            bool found = false;
            for (const auto &rule : rules()) {
                if (entry.error() && rule.severity != "error")
                    continue;
                if (rule.id == "dependency.missing" && lower(text).find("could not find") != string::npos &&
                    (!cmake_error_context || text.find("-- Could") != string::npos))
                    continue;
                size_t position = 0;
                if (!matches(text, rule.pattern, &position))
                    continue;
                save_issue(rule.severity, rule.id, position);
                found = true;
                break; // One primary classification per line; error rules precede warnings.
            }
            if (!found && entry.error())
                save_issue("error", entry.kind + ".error", 0);
        }
        if (input->bad() || (file.is_open() && file.bad()))
            throw std::runtime_error("Error reading log");
        if (!a.exit_file.empty()) {
            std::ifstream codefile(fs::u8path(a.exit_file));
            string code;
            if (!(codefile >> code))
                throw std::runtime_error("Capture did not supply an exit code");
            a.exit_code = integer(code, 0, 4294967295LL);
        }
        Statement update(
            db.db, "UPDATE sources SET "
                   "format=?,lines=?,bytes=?,truncated_lines=?,redacted_lines=?,exit_code=?,success=? WHERE id=?");
        update.bind(1, format);
        update.bind(2, total);
        update.bind(3, bytes);
        update.bind(4, truncated_count);
        update.bind(5, redacted_count);
        if (a.exit_code)
            update.bind(6, static_cast<long long>(*a.exit_code));
        update.bind(7, success ? 1LL : 0LL);
        update.bind(8, id);
        update.step();
        db.exec("COMMIT");
        return id;
    } catch (...) {
        db.exec("ROLLBACK");
        throw;
    }
}

struct Outcome {
    long long errors = 0, warnings = 0, groups = 0;
    string outcome, status, reason;
};
Outcome outcome(Database &db, long long id) {
    Outcome o;
    Statement count(db.db, "SELECT severity,sum(count),count(*) FROM issues WHERE source_id=? GROUP BY severity");
    count.bind(1, id);
    while (count.step()) {
        if (count.str(0) == "error")
            o.errors = count.num(1);
        else
            o.warnings = count.num(1);
        o.groups += count.num(2);
    }
    Statement source(db.db, "SELECT exit_code,success,truncated_lines FROM sources WHERE id=?");
    source.bind(1, id);
    source.step();
    bool known_exit = sqlite3_column_type(source.stmt, 0) != SQLITE_NULL;
    if (o.errors || (known_exit && source.num(0) != 0)) {
        o.outcome = "failed";
        o.status = "error";
        o.reason = o.errors ? "Error evidence detected" : "Process exited with a nonzero code";
    } else if (source.num(2)) {
        o.outcome = "unknown";
        o.status = "unknown";
        o.reason = "Oversized lines were truncated; analysis is incomplete";
    } else if ((known_exit && source.num(0) == 0) || source.num(1)) {
        o.outcome = "passed";
        o.status = o.warnings ? "warning" : "ok";
        o.reason = known_exit ? "Process exited with code 0; no error patterns detected"
                              : "Success marker detected; no error patterns detected";
    } else {
        o.outcome = "unknown";
        o.status = o.warnings ? "warning" : "unknown";
        o.reason = "No exit code or recognized completion marker; success cannot be confirmed";
    }
    return o;
}
string issue_json(const Statement &q, bool compact = false) {
    Statement original(
        sqlite3_db_handle(q.stmt),
        "SELECT l.text FROM issues i JOIN lines l ON l.source_id=i.source_id AND l.number=i.first_line WHERE i.id=?");
    original.bind(1, q.num(0));
    original.step();
    const auto entry = envelope(original.str(0));
    string origin;
    if (!entry.kind.empty())
        origin = ",\"origin\":{\"system\":" + json(entry.kind) + ",\"priority\":" + json(entry.priority) +
                 ",\"tag\":" + json(entry.tag) + ",\"pid\":" + json(entry.pid) + ",\"tid\":" + json(entry.tid) + "}";
    return "{\"id\":" + std::to_string(q.num(0)) + ",\"severity\":" + json(q.str(1)) + ",\"rule\":" + json(q.str(2)) +
           ",\"message\":" + json(clip(q.str(3), compact ? 500 : 1500)) + ",\"message_truncated\":" +
           (q.str(3).size() > (compact ? 500u : 1500u) || q.str(3).rfind("...", 0) == 0 ? "true" : "false") +
           ",\"first_line\":" + std::to_string(q.num(4)) + ",\"last_line\":" + std::to_string(q.num(5)) +
           ",\"count\":" + std::to_string(q.num(6)) + origin + "}";
}
string summary(Database &db, long long id) {
    auto o = outcome(db, id);
    Statement s(db.db, "SELECT name,format,created,lines,bytes,truncated_lines,redacted_lines,exit_code,redaction FROM "
                       "sources WHERE id=?");
    s.bind(1, id);
    s.step();
    string out =
        "{\"schema_version\":1,\"source_id\":" + std::to_string(id) + ",\"name\":" + json(s.str(0)) +
        ",\"format\":" + json(s.str(1)) + ",\"created_at\":" + json(s.str(2)) + ",\"status\":" + json(o.status) +
        ",\"outcome\":" + json(o.outcome) + ",\"reason\":" + json(o.reason) +
        ",\"exit_code\":" + (sqlite3_column_type(s.stmt, 7) == SQLITE_NULL ? "null" : std::to_string(s.num(7))) +
        ",\"lines\":" + std::to_string(s.num(3)) + ",\"input_bytes\":" + std::to_string(s.num(4)) +
        ",\"truncated_lines\":" + std::to_string(s.num(5)) + ",\"redacted_lines\":" + std::to_string(s.num(6)) +
        ",\"redaction_enabled\":" + (s.num(8) ? "true" : "false") + ",\"errors\":" + std::to_string(o.errors) +
        ",\"warnings\":" + std::to_string(o.warnings) + ",\"issue_groups\":" + std::to_string(o.groups) +
        ",\"top_issues\":[";
    Statement q(db.db, "SELECT id,severity,rule,message,first_line,last_line,count FROM issues WHERE source_id=? ORDER "
                       "BY CASE severity WHEN 'error' THEN 0 ELSE 1 END,first_line LIMIT 5");
    q.bind(1, id);
    bool first = true;
    while (q.step()) {
        if (!first)
            out += ',';
        first = false;
        out += issue_json(q, true);
    }
    out += "],\"next\":{\"issues\":\"issues --source " + std::to_string(id) + "\",\"context\":\"context --source " +
           std::to_string(id) + " --line <first_line>\"}}";
    return out;
}
struct Advice {
    string interpretation;
    std::vector<string> checks;
};
Advice advice(const string &rule, const string &message) {
    const auto text = lower(message);
    if (rule == "compiler.kotlin")
        return {"The Kotlin compiler reported a source diagnostic.",
                {"Inspect the .kt file and line, unresolved symbol, imports, and module dependencies before "
                 "investigating later Gradle task failures."}};
    if (rule == "exception.cause")
        return {
            "The exception chain reports an underlying cause.",
            {"Inspect this cause and the adjacent application stack frames; outer exceptions may be consequences."}};
    if (rule == "android.crash" || rule == "logcat.error")
        return {"Android reported a crash or error-level event.",
                {"Inspect the exception chain and stack frames for this PID/tag; distinguish application failures from "
                 "unrelated device services."}};
    if (rule == "vscode.extension" || rule == "vscode.error")
        return {"VS Code or an extension reported an error.",
                {"Check the named extension and the Extension Host/renderer log; correlate with the operation that "
                 "failed."}};
    if (rule == "jetbrains.error")
        return {"The IDE reported an error.",
                {"Inspect the logger, exception cause, and Gradle sync/build output; IDE logs and application logcat "
                 "describe different processes."}};
    if (rule == "gradle.failed")
        return {"Gradle reported a failed task or dependency resolution.",
                {"Inspect the first failed task and its nested cause; use Gradle --stacktrace if the exported output "
                 "omits the exception chain."}};
    if (rule == "cmake.error")
        return {"CMake reported a configuration or script error.",
                {"Inspect the named CMake file/call and following detail; distinguish configuration failures from "
                 "compiler feature probes."}};
    if (rule == "make.target")
        return {"Make could not construct a target or parse a recipe.",
                {"Check the missing target/path or recipe indentation at the reported Makefile line."}};
    if (rule == "xcode.signing")
        return {"Xcode reported a signing or provisioning problem.",
                {"Check the selected development team, signing identity, provisioning profile, bundle identifier, and "
                 "build destination."}};
    if (rule == "dependency.missing") {
        Advice a{"A required header, module, or package could not be found.",
                 {"Check that the dependency is installed in the failing job and that its include or module search "
                  "paths are configured."}};
        if (text.find("sqlite3.h") != string::npos)
            a.checks.push_back("For sqlite3.h on Debian/Ubuntu, check the libsqlite3-dev package; other platforms need "
                               "their SQLite development package.");
        else if (text.find("no module named") != string::npos || text.find("modulenotfounderror") != string::npos)
            a.checks.push_back("Check the Python interpreter, virtual environment, and dependency installation step "
                               "used by this job.");
        return a;
    }
    if (rule == "compiler.error")
        return {"The compiler rejected the source or its build configuration.",
                {"Start with the reported source location and diagnostic; fix it before investigating later "
                 "build-stopped messages."}};
    if (rule == "linker.error")
        return {"The linker could not resolve the program's symbols or libraries.",
                {"Check linked libraries, symbol definitions, library order, and architecture compatibility."}};
    if (rule == "crash.signal" || rule == "crash.apple")
        return {"The log contains crash or sanitizer evidence.",
                {"Inspect the crashed thread, stack trace, or sanitizer report near this line before choosing a fix."}};
    if (rule == "test.failed")
        return {"A test or test suite reported a failure.",
                {"Inspect the failing test's assertion and expected versus actual result; rerun that test in the same "
                 "environment."}};
    if (rule == "exception.unhandled")
        return {"The program reported an unhandled exception or panic.",
                {"Read the surrounding stack trace and final exception message to identify the failing operation."}};
    if (rule == "compiler.warning" || rule == "log.warning" || rule == "github.warning" || rule == "logcat.warning" ||
        rule == "jetbrains.warning" || rule == "vscode.warning")
        return {"The log contains a warning that may need attention.",
                {"Review the warning in context; a warning alone does not establish that the process failed."}};
    if (rule == "build.failed" || rule == "github.error")
        return {"The build or CI runner reported a failure; this message may describe a consequence.",
                {"Look earlier in this job for a compiler error, exception, failed assertion, or missing dependency."}};
    return {"The program emitted an error diagnostic.",
            {"Inspect the surrounding operation and any stack trace; the log alone may not establish the underlying "
             "cause."}};
}
string string_array(const std::vector<string> &items) {
    string out = "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i)
            out += ',';
        out += json(items[i]);
    }
    return out + ']';
}
string prioritized_issues_sql() {
    return "SELECT id,severity,rule,message,first_line,last_line,count FROM issues WHERE source_id=? ORDER BY "
           "CASE severity WHEN 'error' THEN 0 ELSE 1 END, CASE rule "
           "WHEN 'dependency.missing' THEN 0 WHEN 'exception.cause' THEN 0 WHEN 'xcode.signing' THEN 0 WHEN "
           "'make.target' "
           "THEN 1 WHEN 'linker.error' THEN 1 WHEN 'compiler.error' THEN 2 WHEN 'compiler.kotlin' THEN 2 "
           "WHEN 'crash.signal' THEN 3 WHEN 'crash.apple' THEN 3 WHEN 'android.crash' THEN 3 WHEN 'vscode.extension' "
           "THEN "
           "1 WHEN 'cmake.error' THEN 3 WHEN 'gradle.failed' THEN 4 WHEN 'log.error' THEN 4 "
           "WHEN 'test.failed' THEN 5 WHEN 'exception.unhandled' THEN 6 WHEN 'github.error' THEN 7 "
           "WHEN 'build.failed' THEN 8 ELSE 9 END,first_line";
}
string brief_text(Database &db, long long id) {
    const auto o = outcome(db, id);
    Statement source(db.db, "SELECT exit_code,truncated_lines FROM sources WHERE id=?");
    source.bind(1, id);
    source.step();
    string out;
    if (o.errors) {
        Statement problem(db.db, prioritized_issues_sql() + " LIMIT 1");
        problem.bind(1, id);
        problem.step();
        out = "Error in logs is: " + clip(problem.str(3), 500);
        if (o.errors > 1 || o.warnings)
            out += "\n" + std::to_string(o.errors) + (o.errors == 1 ? " error" : " errors") +
                   (o.warnings ? ", " + std::to_string(o.warnings) + (o.warnings == 1 ? " warning" : " warnings") : "");
    } else if (o.outcome == "failed") {
        out = "Error in logs is: process exited with code " + std::to_string(source.num(0)) +
              "; no matching error message was found";
        if (o.warnings)
            out += "\n" + std::to_string(o.warnings) + (o.warnings == 1 ? " warning" : " warnings");
    } else {
        out = o.warnings ? "No errors, " + std::to_string(o.warnings) + (o.warnings == 1 ? " warning" : " warnings")
                         : (o.outcome == "passed" ? "No errors in Logs" : "No errors detected");
    }
    if (o.outcome == "unknown")
        out += "\nOutcome unknown: " + o.reason;
    else if (source.num(1))
        out += "\nAnalysis incomplete: " + std::to_string(source.num(1)) + " oversized lines were truncated";
    return out;
}
string explain(Database &db, long long id, bool human) {
    const auto o = outcome(db, id);
    Statement source(db.db, "SELECT name,lines,truncated_lines,exit_code,format FROM sources WHERE id=?");
    source.bind(1, id);
    source.step();
    string answer;
    if (o.outcome == "failed") {
        const auto format = source.str(4);
        const bool continuous =
            format == "logcat" || format == "android-studio" || format == "jetbrains" || format == "vscode";
        answer = continuous && sqlite3_column_type(source.stmt, 3) == SQLITE_NULL
                     ? "The log contains error or crash evidence."
                     : "The build or run failed.";
    } else if (o.outcome == "passed")
        answer = o.warnings ? "The build or run completed successfully, with warnings."
                            : "The build or run completed successfully; no error patterns were detected.";
    else
        answer = "The log does not establish a successful outcome. " + o.reason + ".";
    // Rank specific diagnostics ahead of generic failure markers. This selects
    // evidence for investigation; it does not prove a root cause.
    Statement q(db.db, prioritized_issues_sql() + " LIMIT 3");
    q.bind(1, id);
    string items = "[", human_details, primary_rule;
    long long primary_line = 0;
    int count = 0;
    while (q.step()) {
        const auto a = advice(q.str(2), q.str(3));
        if (!primary_line) {
            primary_line = q.num(4);
            primary_rule = q.str(2);
            answer += " " + a.interpretation;
        }
        if (count++)
            items += ',';
        string item = issue_json(q, true);
        item.pop_back();
        items += item + ",\"interpretation\":" + json(a.interpretation) +
                 ",\"suggested_checks\":" + string_array(a.checks) + "}";
        human_details += "\n" + string(count == 1 ? "Start here" : "Also reported") + " (log line " +
                         std::to_string(q.num(4)) + ", " + q.str(2) + "):\n  " + clip(q.str(3), 500) + "\n";
        if (count == 1)
            for (const auto &check : a.checks)
                human_details += "  Check: " + check + "\n";
    }
    items += ']';
    std::vector<string> checks;
    if (!primary_line && o.outcome == "failed")
        checks.push_back("The process exited unsuccessfully without a recognized error diagnostic; inspect its final "
                         "output and command-specific exit code.");
    if (o.outcome == "unknown")
        checks.push_back(
            "Obtain the complete job log and its exit code, or capture the command with logsonfire-agent run.");
    if (source.num(2))
        checks.push_back(
            "Oversized lines were truncated; inspect the original log for evidence omitted from analysis.");
    string context = "[", human_context;
    if (primary_line) {
        Statement lines(db.db,
                        "SELECT number,text FROM lines WHERE source_id=? AND number BETWEEN ? AND ? ORDER BY number");
        lines.bind(1, id);
        lines.bind(2, std::max(1LL, primary_line - 2));
        lines.bind(3, primary_line + 2);
        bool first = true;
        while (lines.step()) {
            const auto text = lines.str(1);
            if (!first)
                context += ',';
            first = false;
            context += "{\"line\":" + std::to_string(lines.num(0)) + ",\"text\":" + json(clip(text, 300)) +
                       ",\"truncated\":" + (text.size() > 300 ? "true" : "false") + "}";
            human_context += "  " + std::to_string(lines.num(0)) + " | " + clip(text, 300) + "\n";
        }
    }
    context += ']';
    string crash_context = "[", human_crash_context;
    if (primary_rule == "exception.cause" || primary_rule == "android.crash") {
        Statement frames(db.db, "SELECT text FROM lines WHERE source_id=? AND number=?");
        frames.bind(1, id);
        frames.bind(2, primary_line);
        frames.step();
        const auto root = envelope(frames.str(0));
        Statement nearby_lines(
            db.db, "SELECT number,text FROM lines WHERE source_id=? AND number BETWEEN ? AND ? ORDER BY number");
        nearby_lines.bind(1, id);
        nearby_lines.bind(2, primary_line);
        nearby_lines.bind(3, primary_line + 32);
        int count = 0;
        while (nearby_lines.step() && count < 8) {
            const auto text = nearby_lines.str(1);
            if (text.empty())
                break;
            const auto item = envelope(text);
            if (!root.pid.empty() && item.pid != root.pid)
                continue;
            if (count)
                crash_context += ',';
            ++count;
            crash_context += "{\"line\":" + std::to_string(nearby_lines.num(0)) + ",\"text\":" + json(clip(text, 300)) +
                             ",\"truncated\":" + (text.size() > 300 ? "true" : "false") + "}";
            human_crash_context += "  " + std::to_string(nearby_lines.num(0)) + " | " + clip(text, 300) + "\n";
        }
    } else if (primary_rule == "crash.signal" || primary_rule == "crash.apple") {
        Statement header(db.db, "SELECT number FROM lines WHERE source_id=? AND number BETWEEN ? AND ? AND "
                                "(instr(lower(text),'crashed:')>0 OR instr(lower(text),'stack trace:')>0 OR "
                                "instr(lower(text),'stacktrace:')>0) "
                                "ORDER BY number LIMIT 1");
        header.bind(1, id);
        header.bind(2, primary_line);
        header.bind(3, primary_line + 200);
        if (header.step()) {
            Statement frames(
                db.db, "SELECT number,text FROM lines WHERE source_id=? AND number BETWEEN ? AND ? ORDER BY number");
            frames.bind(1, id);
            frames.bind(2, header.num(0));
            frames.bind(3, header.num(0) + 7);
            bool first = true;
            while (frames.step()) {
                const auto text = frames.str(1);
                if (!first && text.empty())
                    break;
                if (!first)
                    crash_context += ',';
                first = false;
                crash_context += "{\"line\":" + std::to_string(frames.num(0)) + ",\"text\":" + json(clip(text, 300)) +
                                 ",\"truncated\":" + (text.size() > 300 ? "true" : "false") + "}";
                human_crash_context += "  " + std::to_string(frames.num(0)) + " | " + clip(text, 300) + "\n";
            }
        }
    }
    crash_context += ']';
    if (human) {
        string out = answer + "\nSource: " + source.str(0) + " (ID " + std::to_string(id) + ", " +
                     std::to_string(source.num(1)) + " lines)\n" + std::to_string(o.errors) + " error lines, " +
                     std::to_string(o.warnings) + " warning lines, " + std::to_string(o.groups) +
                     " distinct issues.\n" + human_details;
        for (const auto &check : checks)
            out += "\nCheck: " + check + "\n";
        if (!human_context.empty())
            out += "\nEvidence around the first suggested diagnostic:\n" + human_context;
        if (!human_crash_context.empty())
            out += "\nCrashed thread or stack trace:\n" + human_crash_context;
        return out + "\nMore: logsonfire context --source " + std::to_string(id) + " --line " +
               std::to_string(primary_line ? primary_line : 1);
    }
    return "{\"schema_version\":1,\"source_id\":" + std::to_string(id) + ",\"name\":" + json(source.str(0)) +
           ",\"status\":" + json(o.status) + ",\"outcome\":" + json(o.outcome) + ",\"answer\":" + json(answer) +
           ",\"analysis\":\"rule_based\",\"reason\":" + json(o.reason) + ",\"exit_code\":" +
           (sqlite3_column_type(source.stmt, 3) == SQLITE_NULL ? "null" : std::to_string(source.num(3))) +
           ",\"lines\":" + std::to_string(source.num(1)) + ",\"truncated_lines\":" + std::to_string(source.num(2)) +
           ",\"errors\":" + std::to_string(o.errors) + ",\"warnings\":" + std::to_string(o.warnings) +
           ",\"issue_groups\":" + std::to_string(o.groups) + ",\"findings\":" + items +
           ",\"suggested_checks\":" + string_array(checks) + ",\"context\":" + context +
           ",\"crash_context\":" + crash_context + ",\"next\":{\"issues\":\"issues --source " + std::to_string(id) +
           "\",\"context\":\"context --source " + std::to_string(id) + " --line " +
           std::to_string(primary_line ? primary_line : 1) + "\"}}";
}
string issues(Database &db, const Args &a, long long id) {
    string where = " WHERE source_id=?";
    if (!a.severity.empty())
        where += " AND severity=?";
    Statement count(db.db, "SELECT count(*) FROM issues" + where);
    count.bind(1, id);
    if (!a.severity.empty())
        count.bind(2, a.severity);
    count.step();
    Statement q(db.db, "SELECT id,severity,rule,message,first_line,last_line,count FROM issues" + where +
                           " ORDER BY CASE severity WHEN 'error' THEN 0 ELSE 1 END,first_line LIMIT ? OFFSET ?");
    int b = 1;
    q.bind(b++, id);
    if (!a.severity.empty())
        q.bind(b++, a.severity);
    q.bind(b++, a.limit);
    q.bind(b, a.offset);
    string out = a.output == "text" ? ""
                                    : "{\"schema_version\":1,\"source_id\":" + std::to_string(id) +
                                          ",\"total\":" + std::to_string(count.num(0)) + ",\"items\":[";
    long long returned = 0;
    while (q.step()) {
        string item = a.output == "text" ? q.str(1) + " at line " + std::to_string(q.num(4)) + ": " +
                                               clip(q.str(3), 1500) + " (" + std::to_string(q.num(6)) + " occurrences)"
                                         : issue_json(q);
        if (out.size() + item.size() + 256 > MAX_RESPONSE)
            break;
        if (returned++)
            out += a.output == "text" ? '\n' : ',';
        out += item;
    }
    long long next = a.offset + returned;
    if (a.output == "text")
        return (out.empty() ? "No matching issues" : out) +
               (next < count.num(0) ? "\nMore issues available; next offset: " + std::to_string(next) : "");
    return out + "],\"next_offset\":" + (next < count.num(0) ? std::to_string(next) : "null") + "}";
}
string lines(Database &db, const Args &a, long long id, bool search) {
    string sql, predicate;
    if (search) {
        if (a.text.empty())
            throw std::runtime_error("search requires --text");
        if (a.fts && !db.fts)
            throw std::runtime_error("This SQLite build has no FTS5; omit --fts for substring search");
        predicate = a.fts ? " JOIN line_search ON line_search.rowid=l.rowid WHERE l.source_id=? AND line_search MATCH ?"
                          : " WHERE l.source_id=? AND instr(lower(l.text),lower(?)) > 0";
    } else
        predicate = " WHERE l.source_id=? AND l.number BETWEEN ? AND ?";
    sql = "SELECT l.number,l.text FROM lines l" + predicate + " ORDER BY l.number LIMIT ? OFFSET ?";
    Statement q(db.db, sql);
    q.bind(1, id);
    int b = 2;
    if (search)
        q.bind(b++, a.text);
    else {
        q.bind(b++, std::max(1LL, a.line - a.before));
        q.bind(b++, a.line + a.after);
    }
    q.bind(b++, search ? a.limit + 1 : a.before + a.after + 2);
    q.bind(b, search ? a.offset : 0LL);
    string out = a.output == "text" ? "" : "{\"schema_version\":1,\"source_id\":" + std::to_string(id) + ",\"items\":[";
    long long returned = 0;
    bool more = false;
    long long last = 0;
    while (q.step()) {
        const auto text = q.str(1);
        string item = a.output == "text"
                          ? "Line " + std::to_string(q.num(0)) + ": " + clip(text, 2048)
                          : "{\"line\":" + std::to_string(q.num(0)) + ",\"text\":" + json(clip(text, 2048)) +
                                ",\"truncated\":" + (text.size() > 2048 ? "true" : "false") + "}";
        if ((search && returned >= a.limit) || out.size() + item.size() + 256 > MAX_RESPONSE) {
            more = true;
            break;
        }
        if (returned++)
            out += a.output == "text" ? '\n' : ',';
        out += item;
        last = q.num(0);
    }
    if (a.output == "text")
        return (out.empty() ? "No matching log lines" : out) +
               (more ? "\nMore lines available; " + string(search ? "next offset: " : "next line: ") +
                           std::to_string(search ? a.offset + returned : last + 1)
                     : "");
    return out + "],\"has_more\":" + (more ? "true" : "false") +
           ",\"next_offset\":" + (search && more ? std::to_string(a.offset + returned) : "null") +
           ",\"next_line\":" + (!search && more ? std::to_string(last + 1) : "null") + "}";
}
string sources(Database &db, const Args &a) {
    Statement q(db.db, "SELECT id,name,format,lines,created FROM sources ORDER BY id DESC LIMIT ? OFFSET ?");
    q.bind(1, a.limit + 1);
    q.bind(2, a.offset);
    string out = a.output == "text" ? "" : "{\"schema_version\":1,\"items\":[";
    long long returned = 0;
    bool more = false;
    while (q.step()) {
        if (returned >= a.limit || out.size() + 2048 > MAX_RESPONSE) {
            more = true;
            break;
        }
        if (returned++)
            out += a.output == "text" ? '\n' : ',';
        auto o = outcome(db, q.num(0));
        if (a.output == "text") {
            out += std::to_string(q.num(0)) + ": " + clip(q.str(1), 512) + " [" + o.status + ", " +
                   std::to_string(q.num(3)) + " lines]";
            continue;
        }
        out += "{\"source_id\":" + std::to_string(q.num(0)) + ",\"name\":" + json(clip(q.str(1), 512)) +
               ",\"format\":" + json(q.str(2)) + ",\"lines\":" + std::to_string(q.num(3)) +
               ",\"created_at\":" + json(q.str(4)) + ",\"status\":" + json(o.status) +
               ",\"outcome\":" + json(o.outcome) + "}";
    }
    if (a.output == "text")
        return (out.empty() ? "No log sources" : out) +
               (more ? "\nMore sources available; next offset: " + std::to_string(a.offset + returned) : "");
    return out + "],\"next_offset\":" + (more ? std::to_string(a.offset + returned) : "null") + "}";
}
int run_main(int argc, char **argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#else
    umask(0077);
#endif
    Args a;
    if (const char *setting = std::getenv("LOGSONFIRE_OUTPUT"))
        a.output = setting;
    try {
        parse(a, argc, argv);
        if (a.command == "version") {
            if (a.output == "text") {
                std::cout << "logsonfire " LOGSONFIRE_VERSION "\n";
                return 0;
            }
            std::cout << "{\"version\":\"" LOGSONFIRE_VERSION "\",\"schema_version\":1}\n";
            return 0;
        }
        if (a.command == "help" || a.command == "schema") {
            if (a.output == "text") {
                std::cout << "LogsOnFire " LOGSONFIRE_VERSION "\nUsage: logsonfire [--output json|text] COMMAND\n"
                             "Commands: analyze FILE|-, ingest FILE|-, summary, explain, check, sources, issues, "
                             "context, search\n"
                             "Use --source ID for saved evidence; --line N for context; --text TEXT for search.\n"
                             "--human gives a detailed explanation for analyze/explain. --output text gives brief "
                             "plain text.\n"
                             "Settings: LOGSONFIRE_OUTPUT=json|text; LOGSONFIRE_DB=PATH.\n";
                return 0;
            }
            std::cout
                << R"({"schema_version":1,"name":"logsonfire","commands":{"analyze":"FILE|- [--human] [--name NAME] [--format FORMAT] [--exit-code 0..4294967295]","explain":"[--source ID] [--human]","ingest":"FILE|- [--name NAME] [--format auto|generic|github|xcode|crash|python|build|logcat|android-studio|jetbrains|vscode|make|cmake|gradle] [--exit-code 0..4294967295] [--no-redact]","summary":"[--source ID]","check":"[--source ID]; exit 0=passed, 1=failed, 2=unknown/tool error","sources":"[--limit 1..100] [--offset N]","issues":"[--source ID] [--severity error|warning] [--limit 1..100] [--offset N]","context":"[--source ID] --line N [--before 0..49] [--after 0..49]","search":"[--source ID] --text TEXT [--fts] [--limit 1..100] [--offset N]"},"global":"--db PATH or LOGSONFIRE_DB; --output json|text or LOGSONFIRE_OUTPUT; default JSON","defaults":"latest source; JSON by default; analyze/explain support --human; responses <=32768 bytes; summary <=5 issues","workflow":["ingest","summary","issues if needed","context at first_line"]})"
                << '\n';
            return 0;
        }
        const std::vector<string> commands = {"ingest",  "analyze", "explain", "summary", "check",
                                              "sources", "issues",  "context", "search"};
        if (std::find(commands.begin(), commands.end(), a.command) == commands.end())
            throw std::runtime_error("Unknown command: " + a.command);
        Database db(a.db);
        if (a.command == "ingest") {
            const auto id = ingest(db, a);
            std::cout << (a.output == "text" ? brief_text(db, id) : summary(db, id)) << '\n';
            return 0;
        }
        if (a.command == "analyze") {
            const auto id = ingest(db, a);
            std::cout << (a.output == "text" && !a.human ? brief_text(db, id) : explain(db, id, a.human)) << '\n';
            return 0;
        }
        if (a.command == "sources") {
            std::cout << sources(db, a) << '\n';
            return 0;
        }
        auto id = source_id(db, a.source);
        if (a.command == "summary" || a.command == "check") {
            std::cout << (a.output == "text" ? brief_text(db, id) : summary(db, id)) << '\n';
            if (a.command == "check") {
                auto o = outcome(db, id);
                return o.outcome == "passed" ? 0 : (o.outcome == "failed" ? 1 : 2);
            }
        } else if (a.command == "explain")
            std::cout << (a.output == "text" && !a.human ? brief_text(db, id) : explain(db, id, a.human)) << '\n';
        else if (a.command == "issues")
            std::cout << issues(db, a, id) << '\n';
        else
            std::cout << lines(db, a, id, a.command == "search") << '\n';
        return 0;
    } catch (const std::exception &e) {
        if (a.output == "text") {
            std::cout << "LogsOnFire error: " << utf8(clip(e.what(), 1000)) << '\n';
            return 2;
        }
        std::cout << "{\"schema_version\":1,\"error\":{\"code\":\"tool_error\",\"message\":"
                  << json(utf8(clip(e.what(), 1000))) << "}}\n";
        return 2;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t **wide_argv) {
    // Preserve Unicode paths/names rather than using the Windows ANSI code page.
    std::vector<string> storage;
    storage.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        int bytes = WideCharToMultiByte(CP_UTF8, 0, wide_argv[i], -1, nullptr, 0, nullptr, nullptr);
        string arg(static_cast<size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide_argv[i], -1, arg.data(), bytes, nullptr, nullptr);
        arg.pop_back();
        storage.push_back(std::move(arg));
    }
    std::vector<char *> argv;
    for (auto &arg : storage)
        argv.push_back(arg.data());
    return run_main(argc, argv.data());
}
#else
int main(int argc, char **argv) { return run_main(argc, argv); }
#endif
