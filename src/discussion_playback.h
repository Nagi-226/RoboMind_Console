#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QQueue>
#include <QTimer>

class DisplayBridge;
class SpeechBridge;

// Plays one verified discussion in order, independently of model and chat delivery.
class DiscussionPlayback : public QObject {
public:
    DiscussionPlayback(DisplayBridge* display, SpeechBridge* speech, QObject* parent);
    void enqueue(const QString& request, int turn, const QString& text, bool privatePending);
    void cancel(const QString& reason, const QString& request = {});
    QJsonObject status() const;

private:
    struct Line { int turn = 0; QString text; };
    void pump();

    DisplayBridge* display_;
    SpeechBridge* speech_;
    QTimer timer_;
    QQueue<Line> queue_;
    Line line_;
    QJsonArray receipts_;
    QString request_, result_, mediaId_, control_;
    int nextTurn_ = 0;
    qint64 navDeadline_ = 0, lastNav_ = 0;
    bool active_ = false, started_ = false;
};
