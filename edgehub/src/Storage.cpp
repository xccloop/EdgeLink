//这个文件我们来实现将内部消息模型的数据存储进SQLite
#include "Storage.hpp"
#include <cstddef>
#include <sqlite3.h>
#include <string>

Storage::Storage()
{
    db_ = nullptr;
    lastError_.clear();  // 你的原意是清空错误信息，这里用 clear() 更安全
}

bool Storage::open(const std::string &dbPath)
{
    //这句话意味着在dbpath的位置上创建一个数据库，我们后续可以使用db句柄去操控这个数据库，SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE代表着这个数据库可读可写
    if(sqlite3_open_v2(dbPath.c_str(), &db_, SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
    {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    return true;
}

void Storage::close()
{
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool Storage::isOpen() const
{
    if(db_ == nullptr)
    {
        return false;
    }
    return true;
}

bool Storage::createTable()
{
    // 修正：建表必须用 CREATE TABLE，不能是 INSERT
    const char* sql = "CREATE TABLE IF NOT EXISTS messages ("
                      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                      "nodeId INTEGER NOT NULL,"
                      "sequence INTEGER NOT NULL,"
                      "temperature INTEGER,"
                      "temperatureScale INTEGER,"
                      "receivedAtUs INTEGER NOT NULL"
                      ") STRICT;";
    sqlite3_stmt* stmt = nullptr;
    //db是我们的句柄，byte是sql语句的长度我们填入-1意味着让sqlite自己计算
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        lastError_ = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        return false;
    }
    sqlite3_finalize(stmt);
    const char* unique_index_sql =
        "CREATE UNIQUE INDEX IF NOT EXISTS messages_node_sequence_unique "
        "ON messages(nodeId, sequence);";
    rc = sqlite3_prepare_v2(db_, unique_index_sql, -1, &stmt, nullptr);
    if(rc != SQLITE_OK)
    {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    rc = sqlite3_step(stmt);
    if(rc != SQLITE_DONE)
    {
        lastError_ = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        return false;
    }
    sqlite3_finalize(stmt);
    return true;
}

StorageInsertResult Storage::insertMessage(const Message& msg)
{
    const char* sql = "INSERT INTO messages (nodeId, sequence, temperature, temperatureScale, receivedAtUs) "
                      "VALUES (?, ?, ?, ?, ?) ON CONFLICT(nodeId, sequence) DO NOTHING;";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        lastError_ = sqlite3_errmsg(db_);
        return StorageInsertResult::Error;
    }

    sqlite3_bind_int(stmt, 1, msg.nodeId);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(msg.sequence));
    sqlite3_bind_int(stmt, 3, msg.temperature);
    sqlite3_bind_int(stmt, 4, msg.temperatureScale);
    sqlite3_bind_int64(stmt, 5, msg.receivedAtUs);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        lastError_ = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        return StorageInsertResult::Error;
    }

    sqlite3_finalize(stmt);
    return sqlite3_changes(db_) == 1 ? StorageInsertResult::Inserted :
        StorageInsertResult::Duplicate;
}

std::string Storage::getLastError() const
{
    return lastError_;
}

Storage::~Storage()
{
    close();
}
