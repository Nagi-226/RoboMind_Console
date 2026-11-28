#include "group_discussion.h"

#include "ai_status_chat.h"
#include "lanchat_sink.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>

GroupDiscussion::Config GroupDiscussion::Config::load()
{
    const QString name = "robomind-discussion.ini";
    auto path = QDir::current().absoluteFilePath(name);
    if (!QFileInfo::exists(path)) path = QCoreApplication::applicationDirPath() + "/" + name;
    if (!QFileInfo::exists(path)) return {};
    const QSettings ini(path, QSettings::IniFormat);
    return {ini.value("discussion/group_id").toInt(), ini.value("discussion/host_id").toInt()};
}

GroupDiscussion::GroupDiscussion(Config config, LanChatSink* ds, LanChatSink* gpt,
                                 AiStatusChat* whale, AiStatusChat* dragon, QObject* parent)
    : QObject(parent), config_(config), ds_(ds), gpt_(gpt), whale_(whale), dragon_(dragon)
{
    deadline_.setSingleShot(true);
    connect(&deadline_, &QTimer::timeout, this, [this] {
        // Keep an unanswered roster request outstanding until its reply or a reconnect.
        finish(awaitingMembers_ ? "roster_timeout" : "delivery_timeout");
    });
    connect(ds_, &LanChatSink::discussionMessageReceived, this, &GroupDiscussion::receive);
    connect(ds_, &LanChatSink::discussionGroupEvent, this, &GroupDiscussion::groupEvent);
    connect(ds_, &LanChatSink::stateChanged, this, [this] {
        if (!ds_->online()) {
            awaitingMembers_ = false;
            if (!requestId_.isEmpty()) finish("account_offline");
        }
    });
    connect(gpt_, &LanChatSink::stateChanged, this, [this] {
        if (!gpt_->online() && !requestId_.isEmpty()) finish("account_offline");
    });
}

void GroupDiscussion::receive(const QJsonObject& message)
{
    const auto sender = message.value("fromId").toInt();
    const auto text = message.value("msg").toString();
    if (sender == ds_->userId() || sender == gpt_->userId()) {
        if (pendingText_.isEmpty() || text != pendingText_ ||
            sender != (transcript_.size() == 1 ? gpt_->userId() : ds_->userId())) return;
        // Advance only after the server has saved and broadcast the previous turn.
        deadline_.stop();
        transcript_.append(QJsonObject{{"speaker", sender == ds_->userId() ? "DS" : "GPT"},
                                       {"text", pendingAnswer_}});
        emit boardLine(requestId_, static_cast<int>(transcript_.size()), pendingAnswer_);
        pendingText_.clear();
        pendingAnswer_.clear();
        if (transcript_.size() == 3) finish("completed");
        else nextTurn();
        return;
    }
    if (sender != config_.hostId || !requestId_.isEmpty() || awaitingMembers_ ||
        !ds_->online() || !gpt_->online()) return;
    static const QRegularExpression trigger(QStringLiteral("^/讨论\\s+"));
    const auto trimmed = text.trimmed();
    const auto match = trigger.match(trimmed);
    if (!match.hasMatch() || trimmed.toUtf8().size() > 2048) return;
    const auto id = message.value("msg_id").toVariant().toString();
    static const QRegularExpression identifier("^[A-Za-z0-9_.-]{1,63}$");
    if (!identifier.match(id).hasMatch() || seen_.contains(id)) return;
    if (seen_.size() >= 256) seen_.dequeue();
    seen_.enqueue(id);
    requestId_ = id;
    question_ = trimmed.mid(match.capturedEnd()).trimmed();
    awaitingMembers_ = ds_->requestDiscussionMembers();
    if (awaitingMembers_) deadline_.start(5000);
    else finish("account_offline");
}

void GroupDiscussion::groupEvent(const QJsonObject& event)
{
    if (event.value("type").toInt() != 61) {
        if (!requestId_.isEmpty()) finish("roster_changed");
        return;
    }
    if (!awaitingMembers_) return;
    awaitingMembers_ = false;
    if (requestId_.isEmpty()) return;
    deadline_.stop();
    const auto members = event.value("users").toArray();
    QSet<int> ids;
    for (const auto& member : members) ids.insert(member.toObject().value("id").toInt());
    if (event.value("groupId").toInt() != config_.groupId ||
        event.value("hostId").toInt() != config_.hostId || members.size() != 3 ||
        ids != QSet<int>{config_.hostId, ds_->userId(), gpt_->userId()}) {
        finish("roster_mismatch");
        return;
    }
    emit boardLine(requestId_, 0, QStringLiteral("群主提问：") + question_);
    nextTurn();
}

void GroupDiscussion::nextTurn()
{
    const int turn = static_cast<int>(transcript_.size());
    const auto id = requestId_;
    if (id.isEmpty()) return;
    auto* model = turn == 1 ? dragon_ : whale_;
    auto* speaker = turn == 1 ? gpt_ : ds_;
    if (const int wait = model->modelWaitMs(); wait > 0) {
        QTimer::singleShot(wait, Qt::PreciseTimer, this, [this, id] {
            if (id == requestId_) nextTurn();
        });
        return;
    }
    model->requestDiscussionTurn(question_, transcript_,
        [self = QPointer<GroupDiscussion>(this), speaker, id, turn](const QString& answer, bool success) {
            if (!self || self->requestId_ != id) return;
            if (!success) {
                speaker->sendDiscussionReply(QStringLiteral("这轮讨论暂停：%1").arg(answer));
                self->finish("model_failed");
                return;
            }
            self->pendingAnswer_ = answer;
            self->pendingText_ = QStringLiteral("[讨论 %1/3 · #%2]\n%3").arg(turn + 1).arg(id, answer);
            if (speaker->sendDiscussionReply(self->pendingText_)) self->deadline_.start(5000);
            else self->finish("delivery_failed");
        });
}

void GroupDiscussion::finish(const QString& result)
{
    deadline_.stop();
    if (result != "completed") emit boardCancelled(requestId_);
    requestId_.clear();
    question_.clear();
    pendingAnswer_.clear();
    pendingText_.clear();
    transcript_ = {};
    result_ = result;
}

QJsonObject GroupDiscussion::status() const
{
    return {{"group_id", config_.groupId}, {"active", !requestId_.isEmpty()}, {"last_result", result_}};
}
