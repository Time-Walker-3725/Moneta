#include "moneta/storage/book_database.hpp"
#include "book_records.hpp"
#include "file_io.hpp"
#include "sqlite.hpp"

#include <charconv>

namespace moneta {
namespace {
using namespace storage_detail;
using VersionMap = std::map<RecordKey, std::int64_t>;
struct State {
    VersionMap versions;
    BookRecords records;
};

constexpr auto schema = R"sql(
CREATE TABLE revisions(
 id INTEGER PRIMARY KEY, parent INTEGER REFERENCES revisions, restored INTEGER REFERENCES revisions,
 description TEXT NOT NULL, created TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
 depth INTEGER NOT NULL, checkpoint INTEGER NOT NULL DEFAULT 0);
CREATE TABLE versions(
 id INTEGER PRIMARY KEY, origin INTEGER NOT NULL REFERENCES revisions, currency TEXT NOT NULL,
 kind INTEGER NOT NULL, item TEXT NOT NULL, purchase TEXT NOT NULL, payload TEXT NOT NULL);
CREATE INDEX by_origin ON versions(origin,kind,item);
CREATE INDEX by_receipt ON versions(currency,purchase);
CREATE TABLE changes(
 revision INTEGER NOT NULL REFERENCES revisions, older INTEGER REFERENCES versions,
 newer INTEGER REFERENCES versions, CHECK(older IS NOT NULL OR newer IS NOT NULL));
CREATE INDEX by_revision ON changes(revision);
CREATE INDEX by_old_version ON changes(older);
CREATE INDEX by_new_version ON changes(newer);
CREATE TABLE live(version INTEGER PRIMARY KEY REFERENCES versions);
CREATE TABLE checkpoints(revision INTEGER REFERENCES revisions, version INTEGER REFERENCES versions,
 PRIMARY KEY(revision,version)) WITHOUT ROWID;
CREATE TABLE preferred(parent INTEGER PRIMARY KEY REFERENCES revisions, child INTEGER NOT NULL REFERENCES revisions);
CREATE TABLE events(id INTEGER PRIMARY KEY, revision INTEGER NOT NULL REFERENCES revisions,
 action TEXT NOT NULL, created TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
 token TEXT NOT NULL DEFAULT(lower(hex(randomblob(16)))));
CREATE TABLE audit(event INTEGER REFERENCES events, version INTEGER REFERENCES versions,
 direction INTEGER NOT NULL CHECK(direction IN(-1,1)), PRIMARY KEY(event,version,direction)) WITHOUT ROWID;
CREATE INDEX audit_version ON audit(version,event);
INSERT INTO revisions(id,description,depth,checkpoint) VALUES(1,'Empty book',0,1);
INSERT INTO events(revision,action) VALUES(1,'create');
PRAGMA application_id=1297043010;
PRAGMA user_version=1;
)sql";

std::uint64_t unsigned_id(const std::string& s) {
    std::uint64_t id{};
    const auto [end, error] = std::from_chars(s.data(), s.data()+s.size(), id);
    if (error != std::errc{} || end != s.data()+s.size()) throw std::runtime_error("Invalid database record ID");
    return id;
}

State read_state(sqlite3* db, RevisionId past = 0) {
    // Walk back only as far as the nearest checkpoint. Checkpoints share the
    // same immutable version rows, including potentially large receipt text.
    const char* current = R"sql(
        SELECT v.currency,v.kind,v.item,v.purchase,v.payload,v.id FROM live JOIN versions v ON v.id=live.version
    )sql";
    const char* historical = R"sql(
        WITH RECURSIVE path(id,parent,checkpoint) AS (
          SELECT id,parent,checkpoint FROM revisions WHERE id=? UNION ALL
          SELECT r.id,r.parent,r.checkpoint FROM revisions r JOIN path p ON r.id=p.parent WHERE p.checkpoint=0
        ), steps(rev,active,version) AS (
          SELECT p.id,c.version,c.version FROM path p JOIN checkpoints c ON c.revision=p.id WHERE p.checkpoint=1
          UNION ALL SELECT p.id,c.newer,coalesce(c.newer,c.older) FROM path p
             JOIN changes c ON c.revision=p.id WHERE p.checkpoint=0
        )
        SELECT v.currency,v.kind,v.item,v.purchase,v.payload,s.active FROM steps s
        JOIN versions v ON v.id=s.version ORDER BY s.rev
    )sql";
    if (past && !scalar(db,"SELECT id FROM revisions WHERE id=?",past)) throw std::out_of_range("Unknown revision");
    Statement rows{db, past ? historical : current};
    if (past) rows.bind(1, past);
    State result;
    while (rows.step()) {
        if (rows.number(1) < 0 || rows.number(1) > static_cast<int>(RecordKind::note))
            throw std::runtime_error("Unknown SQLite record kind");
        RecordKey key{rows.text(0), static_cast<RecordKind>(rows.number(1)), unsigned_id(rows.text(2))};
        if (!past && result.versions.contains(key)) throw std::runtime_error("Duplicate live record");
        if (auto id = rows.number(5)) {
            result.versions[key] = id;
            result.records[key] = {rows.text(4), unsigned_id(rows.text(3))};
        } else {
            result.versions.erase(key);
            result.records.erase(key);
        }
    }
    return result;
}

void verify_database(sqlite3* db, LoadLimits limits) {
    Transaction tx{db,false};
    Statement check{db,"PRAGMA integrity_check"};
    if (!check.step() || check.text(0) != "ok") throw std::runtime_error("SQLite integrity check failed");
    Statement fk{db,"PRAGMA foreign_key_check"};
    if (fk.step()) throw std::runtime_error("Invalid book history link");
    const auto head = scalar(db,"SELECT revision FROM events ORDER BY id DESC LIMIT 1");
    const auto live = read_state(db);
    if (!head || live.versions != read_state(db,head).versions)
        throw std::runtime_error("Live book disagrees with revision history");
    if (encode_book(decode_book(live.records,limits)) != live.records)
        throw std::runtime_error("Invalid book record or record identity");
    tx.commit();
}

bool permanent(RecordKind kind) {
    return kind == RecordKind::account || kind == RecordKind::investment ||
        kind == RecordKind::location || kind == RecordKind::journal;
}

std::vector<BookRevision> revisions(Statement& q) {
    std::vector<BookRevision> result;
    while (q.step()) result.push_back({q.number(0),q.number(1),q.number(2),q.text(3),q.text(4)});
    return result;
}
} // namespace

struct BookDatabase::Impl {
    Connection conn;
    sqlite3* db;
    LoadLimits limits;
    std::unique_ptr<Book> book;
    RevisionId cached_head{};
    RevisionId head{};
    std::string stamp;
    std::int64_t sequence{};

    Impl(const std::filesystem::path& path, LoadLimits l) : conn(connect(path)), db(conn.get()), limits(l) {
        execute(db,"PRAGMA foreign_keys=ON; PRAGMA trusted_schema=OFF; PRAGMA synchronous=FULL");
        Transaction tx{db,true};
        const auto app = scalar(db,"PRAGMA application_id");
        const auto format = scalar(db,"PRAGMA user_version");
        if (!app && !format && !scalar(db,"SELECT count(*) FROM sqlite_schema")) execute(db,schema);
        else if (app != 1297043010 || format != 1) throw std::runtime_error("Unsupported SQLite book or schema version");
        {
            Statement mode{db,"PRAGMA journal_mode"};
            if (!mode.step() || mode.text(0) == "off" || mode.text(0) == "memory")
                throw std::runtime_error("SQLite book requires durable journaling");
        }
        tx.commit();
        Transaction read{db,false};
        load_current(read);
    }

    void load_current(Transaction& tx) {
        Statement cursor{db,"SELECT id,revision,token FROM events ORDER BY id DESC LIMIT 1"};
        if (!cursor.step()) throw std::runtime_error("Missing book cursor");
        auto seq = cursor.number(0);
        auto revision = cursor.number(1);
        auto next = std::make_unique<Book>(decode_book(read_state(db).records,limits));
        auto token = cursor.text(2);
        next->database_stamp_ = token;
        tx.commit();
        stamp.swap(token);
        cached_head = revision;
        book = std::move(next);
        head = revision;
        sequence = seq;
    }
    void check_writer() const {
        if (sequence != scalar(db,"SELECT max(id) FROM events"))
            throw std::runtime_error("Book changed in another session; refresh before editing");
    }

    std::int64_t event(RevisionId to, const std::string& action) {
        Statement insert{db,"INSERT INTO events(revision,action) VALUES(?,?)"};
        insert.bind(1,to);
        insert.bind(2,action);
        insert.run();
        return sqlite3_last_insert_rowid(db);
    }

    void prefer(RevisionId parent, RevisionId child) {
        Statement q{db,"INSERT OR REPLACE INTO preferred VALUES(?,?)"};
        q.bind(1,parent);
        q.bind(2,child);
        q.run();
    }

    // Switch the materialized state and append immutable, version-linked
    // reversal/application entries for every financial leg affected.
    void apply(RevisionId rev, std::int64_t event_id, bool forward) {
        const auto old = forward ? "older" : "newer";
        const auto next = forward ? "newer" : "older";
        auto selection = [&](const char* column) {
            return std::string("SELECT ") + column + " FROM changes WHERE revision=? AND " + column + " IS NOT NULL";
        };
        Statement remove{db,("DELETE FROM live WHERE version IN (" + selection(old) + ")").c_str()};
        remove.bind(1,rev);
        remove.run();
        Statement insert{db,("INSERT INTO live " + selection(next)).c_str()};
        insert.bind(1,rev);
        insert.run();
        auto audit = [&](const char* column, int direction) {
            Statement q{db,("INSERT INTO audit SELECT ?,id,? FROM versions WHERE kind=? AND id IN (" +
                selection(column) + ")").c_str()};
            q.bind(1,event_id);
            q.bind(2,direction);
            q.bind(3,static_cast<int>(RecordKind::journal));
            q.bind(4,rev);
            q.run();
        };
        audit(old,-1);
        audit(next,1);
    }

    RevisionId commit_state(Transaction& tx, std::string label, const State& current,
        const State& target, RevisionId restored, std::unique_ptr<Book> next_book) {
        if (label.find_first_not_of(" \t\r\n") == std::string::npos) throw std::invalid_argument("Missing action description");
        // Restoring a branch can bring back a different original posting even
        // when its book-local ID and amount happen to be identical.
        if (!restored && current.records == target.records) return head;
        Statement revision{db,"INSERT INTO revisions(parent,restored,description,depth) SELECT id,?,?,depth+1 FROM revisions WHERE id=?"};
        revision.optional_id(1,restored);
        revision.bind(2,label);
        revision.bind(3,head);
        revision.run();
        const auto rev = sqlite3_last_insert_rowid(db);
        Statement add{db,"INSERT INTO versions(origin,currency,kind,item,purchase,payload) VALUES(?,?,?,?,?,?)"};
        Statement change{db,"INSERT INTO changes VALUES(?,?,?)"};
        auto delta = [&](std::int64_t old_id, std::int64_t new_id) {
            change.bind(1,rev);
            change.optional_id(2,old_id);
            change.optional_id(3,new_id);
            change.run();
        };
        for (const auto& [key, row] : target.records) {
            auto old = current.versions.find(key);
            auto old_id = old == current.versions.end() ? 0 : old->second;
            if (restored) {
                const auto version = target.versions.at(key);
                if (old_id != version) delta(old_id,version);
                continue;
            }
            if (old_id && current.records.at(key) == row) continue;
            if (old_id && permanent(key.kind)) throw std::invalid_argument("Cannot rewrite an established account, location, investment or posting");
            add.bind(1,rev);
            add.bind(2,key.currency);
            add.bind(3,static_cast<int>(key.kind));
            add.bind(4,std::to_string(key.id));
            add.bind(5,std::to_string(row.purchase));
            add.bind(6,row.data);
            add.run();
            delta(old_id,sqlite3_last_insert_rowid(db));
        }
        for (const auto& [key, id] : current.versions) {
            if (target.records.contains(key)) continue;
            if (!restored && permanent(key.kind)) throw std::invalid_argument("Cannot remove an established account, location, investment or posting");
            delta(id,0);
        }
        auto seq = event(rev,restored ? "restore" : "edit");
        apply(rev,seq,true);
        prefer(head,rev);
        if (scalar(db,"SELECT depth FROM revisions WHERE id=?",rev) % 256 == 0) {
            Statement snapshot{db,"INSERT INTO checkpoints SELECT ?,version FROM live"};
            snapshot.bind(1,rev);
            snapshot.run();
            Statement mark{db,"UPDATE revisions SET checkpoint=1 WHERE id=?"};
            mark.bind(1,rev);
            mark.run();
        }
        auto token = last_token();
        next_book->database_stamp_ = token;
        tx.commit();
        stamp.swap(token);
        cached_head = rev;
        book = std::move(next_book);
        sequence = seq;
        head = rev;
        return rev;
    }

    std::string last_token() const {
        Statement q{db,"SELECT token FROM events ORDER BY id DESC LIMIT 1"};
        if (!q.step()) throw std::runtime_error("Missing book token");
        return q.text(0);
    }

    bool navigate(bool forward) {
        Transaction tx{db,true};
        check_writer();
        const auto dest = scalar(db, forward ? "SELECT child FROM preferred WHERE parent=?" :
            "SELECT parent FROM revisions WHERE id=?",head);
        if (!dest) return false;
        auto seq = event(dest,forward ? "redo" : "undo");
        apply(forward ? dest : head,seq,forward);
        if (!forward) prefer(dest,head);
        auto token = last_token();
        tx.commit();
        sequence = seq;
        head = dest;
        stamp.swap(token);
        // Retain the existing cache until the next full read. Undo/redo only
        // touch this revision's deltas, even for books with millions of rows.
        return true;
    }
};

BookDatabase::BookDatabase(const std::filesystem::path& path, LoadLimits limits)
    : impl_(std::make_unique<Impl>(path,limits)) {}
BookDatabase::~BookDatabase() = default;
const Book& BookDatabase::book() const {
    if (impl_->cached_head != head()) {
        // Immutable revisions remain readable even if another session has
        // advanced the shared head since our last refresh.
        std::optional<Transaction> read;
        if (sqlite3_get_autocommit(impl_->db)) read.emplace(impl_->db,false);
        const bool current = scalar(impl_->db,"SELECT max(id) FROM events") == impl_->sequence;
        auto state = read_state(impl_->db,current ? 0 : head());
        auto next = std::make_unique<Book>(decode_book(state.records,impl_->limits));
        if (read) read->commit();
        impl_->book = std::move(next);
        impl_->cached_head = head();
    }
    impl_->book->database_stamp_ = impl_->stamp;
    return *impl_->book;
}
RevisionId BookDatabase::head() const noexcept { return impl_->head; }

void BookDatabase::refresh() {
    Transaction tx{impl_->db,false};
    impl_->load_current(tx);
}

RevisionId BookDatabase::edit(std::string description, const std::function<void(Book&)>& action) {
    Transaction tx{impl_->db,true};
    impl_->check_writer();
    auto current = read_state(impl_->db);
    Book candidate = impl_->cached_head == head() ? *impl_->book : decode_book(current.records,impl_->limits);
    action(candidate);
    State target;
    target.records = encode_book(candidate);
    auto next = std::make_unique<Book>(decode_book(target.records,impl_->limits));
    return impl_->commit_state(tx,std::move(description),current,target,0,std::move(next));
}

bool BookDatabase::undo() { return impl_->navigate(false); }
bool BookDatabase::redo() { return impl_->navigate(true); }

RevisionId BookDatabase::restore(RevisionId id, std::string description) {
    Transaction tx{impl_->db,true};
    impl_->check_writer();
    if (id <= 0) throw std::out_of_range("Unknown revision");
    const auto target = read_state(impl_->db,id);
    auto next = std::make_unique<Book>(decode_book(target.records,impl_->limits));
    return impl_->commit_state(tx,std::move(description),read_state(impl_->db),target,id,std::move(next));
}

Book BookDatabase::at(RevisionId id) const {
    if (id <= 0) throw std::out_of_range("Unknown revision");
    return decode_book(read_state(impl_->db,id).records,impl_->limits);
}

std::vector<BookRevision> BookDatabase::history() const {
    Statement q{impl_->db,"SELECT id,parent,restored,description,created FROM revisions ORDER BY id"};
    return revisions(q);
}
std::vector<BookRevision> BookDatabase::receipt_history(Currency currency, NodeId id) const {
    if (!id.value) throw std::invalid_argument("Missing receipt ID");
    Statement q{impl_->db,R"sql(
        WITH ver AS (SELECT id FROM versions WHERE currency=? AND purchase=?),
        edits AS (SELECT revision FROM changes JOIN ver ON older=ver.id UNION
                  SELECT revision FROM changes JOIN ver ON newer=ver.id)
        SELECT id,parent,restored,description,created FROM revisions JOIN edits ON id=revision ORDER BY id
    )sql"};
    q.bind(1,std::string(currency.code()));
    q.bind(2,std::to_string(id.value));
    return revisions(q);
}

std::vector<BookAction> BookDatabase::actions() const {
    Statement q{impl_->db,"SELECT id,revision,action,created FROM events ORDER BY id"};
    std::vector<BookAction> result;
    while (q.step()) result.push_back({q.number(0),q.number(1),q.text(2),q.text(3)});
    return result;
}

namespace {
std::vector<PostingEvent> postings(sqlite3* db, LoadLimits limits, RevisionId origin, LedgerEntryId entry) {
    std::string sql = R"sql(
        SELECT a.event,e.created,v.origin,v.item,v.payload,a.direction,e.action,e.revision FROM audit a
        JOIN events e ON e.id=a.event JOIN versions v ON v.id=a.version
    )sql";
    if (origin) sql += " WHERE v.origin=? AND v.kind=? AND v.item=?";
    sql += " ORDER BY a.event,a.direction,v.id";
    Statement q{db,sql.c_str()};
    if (origin) {
        q.bind(1,origin);
        q.bind(2,static_cast<int>(RecordKind::journal));
        q.bind(3,std::to_string(entry.value));
    }
    std::vector<PostingEvent> result;
    while (q.step()) result.push_back({q.number(0),q.text(1),q.number(2),LedgerEntryId{unsigned_id(q.text(3))},
        decode_posting(q.text(4),limits),static_cast<int>(q.number(5)),q.text(6),q.number(7)});
    return result;
}
} // namespace

std::vector<PostingEvent> BookDatabase::posting_history() const {
    return postings(impl_->db,impl_->limits,0,{});
}
std::vector<PostingEvent> BookDatabase::posting_history(RevisionId origin, LedgerEntryId entry) const {
    if (origin <= 0 || !entry.value) throw std::invalid_argument("Posting origin and ID required");
    return postings(impl_->db,impl_->limits,origin,entry);
}
void BookDatabase::verify() const { verify_database(impl_->db,impl_->limits); }

RevisionId BookDatabase::import_legacy(const std::filesystem::path& source) {
    if (head() != 1 || impl_->sequence != 1) throw std::logic_error("Import requires a fresh database");
    std::ifstream input{source,std::ios::binary};
    if (!input) throw std::runtime_error("Cannot open legacy book");
    auto loaded = Book::read(input,impl_->limits);
    return edit("Import legacy book",[&](Book& b) { b = std::move(loaded); });
}

void BookDatabase::save_snapshot(const Book& source) {
    if (source.database_stamp_ != impl_->stamp && !(head() == 1 && impl_->sequence == 1))
        throw std::runtime_error("Detached book is stale or belongs to another database");
    edit("Save book",[&](Book& target) { target = source; });
    source.database_stamp_ = impl_->stamp;
}

Book BookDatabase::read_snapshot(const std::filesystem::path& path, LoadLimits limits) {
    auto conn = connect(path,SQLITE_OPEN_READONLY);
    sqlite3* db = conn.get();
    Transaction tx{db,false};
    if (scalar(db,"PRAGMA application_id") != 1297043010 || scalar(db,"PRAGMA user_version") != 1)
        throw std::runtime_error("Unsupported SQLite book or schema version");
    auto snapshot = decode_book(read_state(db).records,limits);
    Statement token{db,"SELECT token FROM events ORDER BY id DESC LIMIT 1"};
    if (!token.step()) throw std::runtime_error("Missing book cursor");
    snapshot.database_stamp_ = token.text(0);
    tx.commit();
    return snapshot;
}

void BookDatabase::backup(const std::filesystem::path& path) const {
    TemporaryDirectory temporary{path};
    {
        auto target = connect(temporary.file());
        auto* job = sqlite3_backup_init(target.get(),"main",impl_->db,"main");
        if (!job) throw std::runtime_error(sqlite3_errmsg(target.get()));
        const auto rc = sqlite3_backup_step(job,-1);
        const auto finished = sqlite3_backup_finish(job);
        if (rc != SQLITE_DONE) sql_check(target.get(),rc);
        sql_check(target.get(),finished);
        verify_database(target.get(),impl_->limits);
    }
    // Atomic, exclusive publication on the destination filesystem.
    std::filesystem::create_hard_link(temporary.file(),path);
}
} // namespace moneta
