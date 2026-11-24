#include "moneta/storage/book_database.hpp"

#include <sqlite3.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#ifdef __unix__
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace moneta;
using namespace std::chrono;
namespace {
const Currency usd{"USD", 2};
const Date today = year{2026}/November/25;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Action> void rejects(Action action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Operation should have failed");
}

void sql(const std::filesystem::path& path, const char* query) {
    sqlite3* db = nullptr;
    if (sqlite3_open(path.string().c_str(), &db) != SQLITE_OK) throw std::runtime_error("Open failed");
    const auto rc = sqlite3_exec(db,query,nullptr,nullptr,nullptr);
    sqlite3_close(db);
    check(rc == SQLITE_OK,"Test SQL failed");
}

void history_and_recovery(const std::filesystem::path& dir) {
    const auto path = dir/"main.sqlite";
    AccountId cash;
    LocationId shop;
    LedgerEntryId payment;
    RevisionId opening, paid, split;
    NodeId purchase;
    {
        BookDatabase db{path};
        check(!db.undo() && !db.redo(), "Empty history boundaries");
        opening = db.edit("Open account",[&](Book& b) {
            cash = b.open_account("Bank",usd);
            b.opening(cash, Money{1000,usd},today);
            shop = b.add_location("Book shop");
        });
        paid = db.edit("Buy books",[&](Book& b) {
            payment = b.spend(cash,Money{500,usd},today,"Books",shop);
            purchase = b.entry(payment).purchase;
        });
        check(db.book().account(cash).balance == Money{500,usd},"Posted immediately");
        split = db.edit("Split receipt",[&](Book& b) {
            auto tree = b.expenses(usd);
            const auto tag = tree.create_tag("Reading");
            tree.split(purchase,{{"First",Money{200,usd},"\nquoted \"value\"",{tag}},
                {"Second",Money{300,usd},std::string("a\0b",3),{}}});
            b.replace_expenses(std::move(tree));
        });
        auto history = db.receipt_history(usd,purchase);
        check(history.size()==2 && history[0].id==paid && history[1].id==split,"Indexed receipt history");
        check(db.undo() && db.head()==paid && db.book().account(cash).balance==Money{500,usd},"Undo split");
        check(db.undo() && db.head()==opening && db.book().account(cash).balance==Money{1000,usd},"Reverse payment");
        check(db.book().expenses(usd).transaction_count()==0,"Reversed purchase leaves active reports");
        auto trace = db.posting_history(paid,payment);
        check(trace.size()==2 && trace.back().active_revision==opening && trace.back().action=="undo",
            "Per-posting lookup traces its cash reversal");
        check(db.actions().back().action=="undo","Navigation history includes non-financial actions");
        auto audit = db.posting_history();
        check(audit.size()==3 && audit[2].direction==-1 && audit[2].origin==paid &&
            audit[2].entry_id==payment && audit[2].posting.change==-500,"Linked financial reversal");
    }
    BookDatabase db{path};
    check(db.redo() && db.head()==paid && db.redo() && db.head()==split && !db.redo(),"Redo chain survives reopen");
    check(db.book().expenses(usd).node(purchase).children.size()==2,"Recovered split");
    check(db.book().expenses(usd).note(db.book().expenses(usd).node(purchase).children.back()) ==
        std::string("a\0b",3),"Embedded NUL survives SQLite round trip");
    db.undo();
    db.undo();
    const auto branch = db.edit("Different purchase",[&](Book& b) { b.spend(cash,Money{10,usd},today,"Pen"); });
    check(db.book().account(cash).balance==Money{990,usd} && db.at(split).account(cash).balance==Money{500,usd},
        "Both branches available");
    const auto restored = db.restore(split,"Restore books");
    check(db.history().back().restored_from==split && db.history().back().parent==branch,"Restore appends revision");
    check(db.book().account(cash).balance==Money{500,usd} && db.undo() && db.head()==branch,"Restore is undoable");
    db.backup(dir/"backup.sqlite");
    rejects([&]{ db.backup(dir/"backup.sqlite"); });
    BookDatabase backup{dir/"backup.sqlite"};
    check(backup.redo() && backup.head()==restored && backup.book().account(cash).balance==Money{500,usd},
        "Backup carries complete history and redo cursor");
    check(db.head()==branch,"Backup is independent");
    BookDatabase other{path};
    db.redo();
    db.undo(); // Same head again, but the session has advanced.
    rejects([&]{ other.edit("Stale",[](Book& b){ b.add_location("x"); }); });
    other.refresh();
    other.edit("New location",[](Book& b){ b.add_location("x"); });
    db.refresh();
    db.verify();
    auto before = db.history().size();
    auto balance = db.book().account(cash).balance;
    rejects([&]{ db.edit("Rejected action",[&](Book& b) {
        b.income(cash,Money{100,usd},today,"First part");
        b.spend(cash,Money{5,usd},today,"Invalid place",LocationId{999});
    }); });
    check(db.book().account(cash).balance==balance && db.history().size()==before,"Domain failure is atomic");
    sql(path,"CREATE TRIGGER test_failure BEFORE INSERT ON audit BEGIN SELECT RAISE(ABORT,'test failure'); END");
    rejects([&]{ db.edit("Storage failure",[&](Book& b){ b.spend(cash,Money{5,usd},today,"valid"); }); });
    sql(path,"DROP TRIGGER test_failure");
    db.refresh();
    check(db.book().account(cash).balance==balance && db.history().size()==before,"Database failure rolls back all rows");
    auto original = db.head();
    check(db.edit("No change",[](Book&){})==original && db.head()==original,"Empty actions do not grow history");
    rejects([&]{ db.at(0); });
    rejects([&]{ db.restore(99999,"Missing revision"); });
}

#ifdef __unix__
void interrupted_write(const std::filesystem::path& dir) {
    auto path = dir/"interrupted.sqlite";
    {
        BookDatabase db{path};
        db.edit("Persist cash",[](Book& b) {
            auto cash = b.open_account("Cash",usd);
            b.opening(cash,Money{500,usd},today);
        });
    }
    // No inherited SQLite connections. Force dirty pages to disk, then exit
    // without COMMIT, rollback, finalizers or a clean connection close.
    const auto pid = fork();
    if (pid < 0) throw std::runtime_error("fork failed");
    if (!pid) {
        sqlite3* handle = nullptr;
        if (sqlite3_open(path.c_str(),&handle) != SQLITE_OK ||
            sqlite3_exec(handle,"BEGIN IMMEDIATE; DELETE FROM live",nullptr,nullptr,nullptr) != SQLITE_OK ||
            sqlite3_db_cacheflush(handle) != SQLITE_OK) _exit(2);
        _exit(0);
    }
    int status = 0;
    check(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0,"Crash fixture failed");
    BookDatabase recovered{path};
    recovered.verify();
    check(recovered.book().account(AccountId{1}).balance==Money{500,usd},"Recovered the last committed payment");
}
#endif

void imports_and_checkpoints(const std::filesystem::path& dir) {
    Book legacy;
    auto cash = legacy.open_account("Cash",usd);
    auto yen = legacy.open_account("Yen",Currency{"JPY",0});
    legacy.transfer(cash,yen,Money{200,usd},Money{300,Currency{"JPY",0}},today);
    legacy.invest(cash,Money{123,usd},500,today,"CD");
    auto s = legacy.add_schedule(EntryKind::expense,cash,Money{10,usd},"Daily",today,Frequency::daily);
    legacy.run_due(s,today);
    const auto path = dir/"legacy.moneta";
    legacy.export_legacy(path);
    const auto snapshot_file = dir/"saved.book";
    legacy.save(snapshot_file);
    std::ifstream signature{snapshot_file,std::ios::binary};
    char header[15];
    check(signature.read(header,sizeof(header)) && std::string_view(header,15)=="SQLite format 3","Default format is SQLite");
    auto stale = Book::load(snapshot_file);
    legacy.add_location("After save");
    legacy.save(snapshot_file);
    check(Book::load(snapshot_file).locations().size()==1,"Repeated save preserves SQLite state");
    rejects([&]{ stale.save(snapshot_file); });
    rejects([&]{ legacy.export_legacy(snapshot_file); });
    BookDatabase db{dir/"import.sqlite"};
    db.import_legacy(path);
    check(db.book().account(cash).balance==legacy.account(cash).balance &&
        db.book().schedule(s).next==legacy.schedule(s).next && db.book().investments().size()==1,"Imported entire book");
    check(Book::load(path).journal().size()==3,"Legacy file remains readable");
    rejects([&]{ db.import_legacy(path); });
    // Beyond the checkpoint boundary, edit a tiny book on the same branch.
    BookDatabase small{dir/"checkpoints.sqlite"};
    AccountId a;
    small.edit("Open",[&](Book& b){ a=b.open_account("Cash",usd); });
    for (int i=0;i<257;++i) small.edit("Receive",[&](Book& b){ b.income(a,Money{1,usd},today,"Income"); });
    check(small.at(257).account(a).balance==Money{255,usd} &&
        small.at(258).account(a).balance==Money{256,usd},"Historical reconstruction across checkpoint");
    // A structurally valid SQLite database can still disagree with its journal.
    db.backup(dir/"damaged.sqlite");
    sql(dir/"damaged.sqlite","DELETE FROM live");
    BookDatabase damaged{dir/"damaged.sqlite"};
    rejects([&]{ damaged.verify(); });
    rejects([&]{ damaged.backup(dir/"bad-backup.sqlite"); });
    check(!std::filesystem::exists(dir/"bad-backup.sqlite"),"Failed backup never publishes");
    sql(dir/"foreign.db","CREATE TABLE unrelated(x)");
    rejects([&]{ BookDatabase foreign{dir/"foreign.db"}; });
}

} // namespace
int main() {
    auto dir = std::filesystem::temp_directory_path()/
        ("moneta-sqlite-test-"+std::to_string(steady_clock::now().time_since_epoch().count()));
    try {
        if (!std::filesystem::create_directory(dir)) throw std::runtime_error("Test directory unavailable");
        history_and_recovery(dir);
        imports_and_checkpoints(dir);
#ifdef __unix__
        interrupted_write(dir);
#endif
        std::filesystem::remove_all(dir);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
