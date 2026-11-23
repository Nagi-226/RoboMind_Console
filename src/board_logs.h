#pragma once
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QThread>
#include <memory>

// 串口原文先清理、脱敏，再进入这份有上限的缓存；所有出口只读它。
class BoardLogBuffer {
public:
    void feed(const QByteArray& bytes);
    void finish();
    void restore(const QJsonArray& rows);
    void setSource(const QString& session,const QString& port) { session_=session;port_=port; }
    QJsonArray rows() const { return rows_; }
    QJsonObject counts() const;
    static QString sanitize(QString text);
    static QString utf8Limit(const QString& text, int bytes);
private:
    void line();
    void append(QJsonObject row);
    QByteArray partial_;
    QString session_,port_;
    bool overflow_ = false;
    QJsonArray rows_;
    qint64 sequence_ = 0, bytes_ = 0, evicted_ = 0, truncated_ = 0;
};

class BoardLogs : public QObject {
public:
    explicit BoardLogs(const QString& directory, QObject* parent = nullptr);
    ~BoardLogs() override;
    void start(const QString& port);
    void stop();
    QJsonObject status() const;
    QString reply(const QString& filter) const;
private:
    struct Shared;
    class Worker;
    std::shared_ptr<Shared> shared_;
    Worker* worker_;
    QThread thread_;
};
