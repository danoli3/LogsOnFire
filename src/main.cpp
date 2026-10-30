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
std::regex rx(const string &s) { return std::regex(s, std::regex::icase | std::regex::optimize); }
const std::vector<Rule> &rules() {
    static const std::vector<Rule> items = {
        {"github.error", "error", rx("::error(?:[ :]|::)|##\\[error\\]")},
        {"github.warning", "warning", rx("::warning(?:[ :]|::)|##\\[warning\\]")},
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
         rx("(?:^|[[:space:]])FAIL(?:ED)?(?:[ :]|$)|[1-9][0-9]* (?:failed|failures|errors)(?:[, .]|$)|test case .+ "
            "failed|The following tests FAILED")},
        {"log.error", "error",
         rx("\\b(?:ERROR|FATAL|CRITICAL)\\b[[:space:]]*[:\\]]|\\[(?:ERROR|FATAL|CRITICAL)\\]|\"(?:level|severity|"
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
        if (!contains_any(lower(s), {"authorization", "bearer", "password", "passwd", "pwd", "token", "secret",
                                     "api_key", "api-key", "access_key", "access-key", "ghp_", "gho_", "ghu_", "ghs_",
                                     "ghr_", "github_pat_", "akia", "://"}))
            return s;
        static const auto auth = rx("(authorization[\\\"']?[[:space:]]*[:=][[:space:]]*[\\\"']?(?:bearer|basic)[[:"
                                    "space:]]+)[^[:space:]\\\"',;]+");
        static const auto bearer = rx("(\\bbearer[[:space:]]+)[A-Za-z0-9._~+/-]+=*");
        static const auto assigned =
            rx("((?:password|passwd|pwd|token|secret|api[_-]?key|access[_-]?key|client[_-]?secret)[\\\"']?[[:space:]]*["
               ":=][[:space:]]*)(?:\"[^\"]*\"|'[^']*'|[^[:space:]&,;]+)");
        static const auto tokens =
            rx("\\b(?:gh[pousr]_[A-Za-z0-9_]{16,}|github_pat_[A-Za-z0-9_]{16,}|AKIA[A-Z0-9]{16})\\b");
        static const auto credentials = rx("(https?://)[^/@[:space:]]+:[^/@[:space:]]+@");
        s = std::regex_replace(s, auth, "$1[REDACTED]");
        s = std::regex_replace(s, bearer, "$1[REDACTED]");
        s = std::regex_replace(s, assigned, "$1[REDACTED]");
        s = std::regex_replace(s, tokens, "[REDACTED]");
        return std::regex_replace(s, credentials, "$1[REDACTED]@");
    }
};
string fingerprint(string s) {
    static const auto timestamp = rx("^[0-9]{4}-[0-9]{2}-[0-9]{2}[T ][0-9:.+Z-]+[[:space:]]*");
    static const auto address = rx("\\b0x[0-9a-f]+\\b");
    static const auto whitespace = rx("[[:space:]]+");
    s = std::regex_replace(s, timestamp, "");
    s = std::regex_replace(s, address, "<address>");
    return std::regex_replace(s, whitespace, " ");
}
bool success_line(const string &s) {
    if (!contains_any(lower(s), {"succeeded", "success", "passed", "ok", "exit code 0", "tests run:"}))
        return false;
    static const auto pattern =
        rx("\\*\\* (?:BUILD|TEST|ARCHIVE|EXPORT) SUCCEEDED \\*\\*|BUILD SUCCESSFUL|BUILD SUCCESS(?:[[:space:]]|$)|100% "
           "tests passed|^OK(?:[[:space:]]|$)|Process completed with exit code 0(?:[^0-9]|$)|^Tests run: [0-9]+, "
           "Failures: 0, Errors: 0|^[0-9]+ passed(?: in |$)");
    return std::regex_search(s, pattern);
}
string detect_format(const string &s, string current) {
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
    if (s.find("CMake") != string::npos || s.find("ninja:") != string::npos)
        return "build";
    return current;
}

struct Args {
    string command, db, file, name, format = "auto", text, exit_file;
    long long source = 0, limit = 10, offset = 0, line = 1, before = 3, after = 3;
    std::optional<int> exit_code;
    bool redact = true, fts = false;
    string severity;
};
long long integer(const string &s, long long min, long long max) {
    size_t consumed = 0;
    long long n = std::stoll(s, &consumed);
    if (consumed != s.size() || n < min || n > max)
        throw std::runtime_error("Integer out of range: " + s);
    return n;
}
Args parse(int argc, char **argv) {
    Args a;
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
        else if (s == "--text")
            a.text = value();
        else if (s == "--severity")
            a.severity = value();
        else if (s == "--exit-code")
            a.exit_code = static_cast<int>(integer(value(), 0, 255));
        else if (s == "--exit-code-file")
            a.exit_file = value();
        else if (s == "--no-redact")
            a.redact = false;
        else if (s == "--fts")
            a.fts = true;
        else if (s == "--help" || s == "-h")
            a.command = "help";
        else if (s == "--version")
            a.command = "version";
        else if (a.command.empty())
            a.command = s;
        else if (a.command == "ingest" && a.file.empty() && (s == "-" || s[0] != '-'))
            a.file = s;
        else
            throw std::runtime_error("Unknown argument: " + s);
    }
    if (a.command.empty())
        a.command = "help";
    if (a.format != "auto" && a.format != "generic" && a.format != "github" && a.format != "xcode" &&
        a.format != "crash" && a.format != "python" && a.format != "build")
        throw std::runtime_error("Unknown format");
    if (!a.severity.empty() && a.severity != "error" && a.severity != "warning")
        throw std::runtime_error("Severity must be error or warning");
    if (a.text.size() > 1024 || a.name.size() > 512)
        throw std::runtime_error("Text/name exceeds input limit");
    return a;
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
            if (a.format == "auto")
                format = detect_format(text, format);
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
            if (!contains_any(lower(text), {"error",          "fatal",         "critical",
                                            "warn",           "fail",          "exception",
                                            "crash",          "segmentation",  "segfault",
                                            "bus error",      "sigsegv",       "sigabrt",
                                            "sigbus",         "sigill",        "sigfpe",
                                            "sanitizer",      "traceback",     "panic",
                                            "undefined",      "unresolved",    "duplicate symbol",
                                            "cannot find -l", "build stopped", "termination"}))
                continue;
            for (const auto &rule : rules()) {
                if (!std::regex_search(text, rule.pattern))
                    continue;
                issue.bind(1, id);
                issue.bind(2, rule.severity);
                issue.bind(3, rule.id);
                issue.bind(4, clip(text, 2000));
                issue.bind(5, fingerprint(text));
                issue.bind(6, total);
                issue.bind(7, total);
                issue.step();
                issue.reset();
                break; // One primary classification per line; error rules precede warnings.
            }
        }
        if (input->bad() || (file.is_open() && file.bad()))
            throw std::runtime_error("Error reading log");
        if (!a.exit_file.empty()) {
            std::ifstream codefile(fs::u8path(a.exit_file));
            string code;
            if (!(codefile >> code))
                throw std::runtime_error("Capture did not supply an exit code");
            a.exit_code = static_cast<int>(integer(code, 0, 255));
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
    return "{\"id\":" + std::to_string(q.num(0)) + ",\"severity\":" + json(q.str(1)) + ",\"rule\":" + json(q.str(2)) +
           ",\"message\":" + json(clip(q.str(3), compact ? 500 : 1500)) +
           ",\"message_truncated\":" + (q.str(3).size() > (compact ? 500u : 1500u) ? "true" : "false") +
           ",\"first_line\":" + std::to_string(q.num(4)) + ",\"last_line\":" + std::to_string(q.num(5)) +
           ",\"count\":" + std::to_string(q.num(6)) + "}";
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
    string out = "{\"schema_version\":1,\"source_id\":" + std::to_string(id) +
                 ",\"total\":" + std::to_string(count.num(0)) + ",\"items\":[";
    long long returned = 0;
    while (q.step()) {
        string item = issue_json(q);
        if (out.size() + item.size() + 256 > MAX_RESPONSE)
            break;
        if (returned++)
            out += ',';
        out += item;
    }
    long long next = a.offset + returned;
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
    string out = "{\"schema_version\":1,\"source_id\":" + std::to_string(id) + ",\"items\":[";
    long long returned = 0;
    bool more = false;
    long long last = 0;
    while (q.step()) {
        const auto text = q.str(1);
        string item = "{\"line\":" + std::to_string(q.num(0)) + ",\"text\":" + json(clip(text, 2048)) +
                      ",\"truncated\":" + (text.size() > 2048 ? "true" : "false") + "}";
        if ((search && returned >= a.limit) || out.size() + item.size() + 256 > MAX_RESPONSE) {
            more = true;
            break;
        }
        if (returned++)
            out += ',';
        out += item;
        last = q.num(0);
    }
    return out + "],\"has_more\":" + (more ? "true" : "false") +
           ",\"next_offset\":" + (search && more ? std::to_string(a.offset + returned) : "null") +
           ",\"next_line\":" + (!search && more ? std::to_string(last + 1) : "null") + "}";
}
string sources(Database &db, const Args &a) {
    Statement q(db.db, "SELECT id,name,format,lines,created FROM sources ORDER BY id DESC LIMIT ? OFFSET ?");
    q.bind(1, a.limit + 1);
    q.bind(2, a.offset);
    string out = "{\"schema_version\":1,\"items\":[";
    long long returned = 0;
    bool more = false;
    while (q.step()) {
        if (returned >= a.limit || out.size() + 2048 > MAX_RESPONSE) {
            more = true;
            break;
        }
        if (returned++)
            out += ',';
        auto o = outcome(db, q.num(0));
        out += "{\"source_id\":" + std::to_string(q.num(0)) + ",\"name\":" + json(clip(q.str(1), 512)) +
               ",\"format\":" + json(q.str(2)) + ",\"lines\":" + std::to_string(q.num(3)) +
               ",\"created_at\":" + json(q.str(4)) + ",\"status\":" + json(o.status) +
               ",\"outcome\":" + json(o.outcome) + "}";
    }
    return out + "],\"next_offset\":" + (more ? std::to_string(a.offset + returned) : "null") + "}";
}
int run_main(int argc, char **argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#else
    umask(0077);
#endif
    try {
        auto a = parse(argc, argv);
        if (a.command == "version") {
            std::cout << "{\"version\":\"" LOGSONFIRE_VERSION "\",\"schema_version\":1}\n";
            return 0;
        }
        if (a.command == "help" || a.command == "schema") {
            std::cout
                << R"({"schema_version":1,"name":"logsonfire","commands":{"ingest":"FILE|- [--name NAME] [--format auto|generic|github|xcode|crash|python|build] [--exit-code 0..255] [--no-redact]","summary":"[--source ID]","check":"[--source ID]; exit 0=passed, 1=failed, 2=unknown/tool error","sources":"[--limit 1..100] [--offset N]","issues":"[--source ID] [--severity error|warning] [--limit 1..100] [--offset N]","context":"[--source ID] --line N [--before 0..49] [--after 0..49]","search":"[--source ID] --text TEXT [--fts] [--limit 1..100] [--offset N]"},"global":"--db PATH or LOGSONFIRE_DB; default .logsonfire/logs.db","defaults":"latest source; JSON only; responses <=32768 bytes; summary <=5 issues","workflow":["ingest","summary","issues if needed","context at first_line"]})"
                << '\n';
            return 0;
        }
        const std::vector<string> commands = {"ingest", "summary", "check", "sources", "issues", "context", "search"};
        if (std::find(commands.begin(), commands.end(), a.command) == commands.end())
            throw std::runtime_error("Unknown command: " + a.command);
        Database db(a.db);
        if (a.command == "ingest") {
            std::cout << summary(db, ingest(db, a)) << '\n';
            return 0;
        }
        if (a.command == "sources") {
            std::cout << sources(db, a) << '\n';
            return 0;
        }
        auto id = source_id(db, a.source);
        if (a.command == "summary" || a.command == "check") {
            std::cout << summary(db, id) << '\n';
            if (a.command == "check") {
                auto o = outcome(db, id);
                return o.outcome == "passed" ? 0 : (o.outcome == "failed" ? 1 : 2);
            }
        } else if (a.command == "issues")
            std::cout << issues(db, a, id) << '\n';
        else
            std::cout << lines(db, a, id, a.command == "search") << '\n';
        return 0;
    } catch (const std::exception &e) {
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
