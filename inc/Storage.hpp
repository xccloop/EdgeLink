#include <sqlite3.h>
#include "Message.hpp"
#include <string>

class Storage {
public:
    // ---------- 构造与析构 ----------
    Storage();
    ~Storage();

    // 禁止拷贝（防止误用）
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    // ---------- 生命周期管理 ----------
    // 打开数据库（文件路径，若不存在则自动创建）
    bool open(const std::string& dbPath);
    
    // 关闭数据库
    void close();
    
    // 检查数据库是否已打开
    bool isOpen() const;

    // ---------- 表结构初始化 ----------
    // 创建 messages 表（如果不存在则创建，带严格模式）
    bool createTable();

    // ---------- 核心数据操作（预编译语句） ----------
    // 插入一条 Message 数据（返回 true 表示成功）
    bool insertMessage(const Message& msg);

    // ---------- 错误信息获取 ----------
    // 获取最后一次操作的错误描述
    std::string getLastError() const;

private:
    // ---------- 私有成员变量 ----------
    sqlite3* db_;          // SQLite 连接句柄
    std::string lastError_; // 缓存最后一次错误信息

    // ---------- 私有辅助函数 ----------
    // 设置错误信息（内部使用）
    void setLastError(const std::string& msg);
    void setLastErrorFromDb(); // 从 sqlite3_errmsg(db_) 获取错误
};