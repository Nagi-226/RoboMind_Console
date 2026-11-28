#include "discussion_playback.h"

#include "display_bridge.h"
#include "speech_bridge.h"

#include <QDateTime>
#include <QUuid>

DiscussionPlayback::DiscussionPlayback(DisplayBridge* display, SpeechBridge* speech, QObject* parent)
    : QObject(parent), display_(display), speech_(speech)
{
    timer_.setInterval(100);
    connect(&timer_, &QTimer::timeout, this, &DiscussionPlayback::pump);
}

void DiscussionPlayback::enqueue(const QString& request, int turn, const QString& text, bool privatePending)
{
    if (turn == 0) {
        cancel("replaced");
        request_ = request;
        receipts_ = {};
        nextTurn_ = 0;
        if (privatePending || speech_->busy()) {
            result_ = "private_chat_active";
            return;
        }
        result_ = "waiting";
        active_ = true;
    }
    if (!active_ || request != request_ || turn != nextTurn_ || turn > 3) return;
    ++nextTurn_;
    queue_.enqueue({turn, text});
    timer_.start();
    pump();
}

void DiscussionPlayback::cancel(const QString& reason, const QString& request)
{
    if (!active_ || (!request.isEmpty() && request != request_)) return;
    if (!mediaId_.isEmpty() && speech_->status().value("active_id").toString() == mediaId_) speech_->cancel();
    queue_.clear();
    line_ = {};
    mediaId_.clear();
    control_.clear();
    timer_.stop();
    active_ = started_ = false;
    result_ = reason;
}

void DiscussionPlayback::pump()
{
    const auto board = display_->status();
    if (!board.value("terminal_ready").toBool()) { cancel("board_unavailable"); return; }
    if (!board.value("immersion_supported").toBool() || !board.value("dragon_supported").toBool() ||
        !board.value("navigation").toBool() || !board.value("pcm_supported").toBool()) {
        cancel("board_media_unsupported");
        return;
    }
    if (mediaId_.isEmpty()) {
        if (queue_.isEmpty()) {
            result_ = "waiting";
            timer_.stop();
            return;
        }
        line_ = queue_.dequeue();
        mediaId_ = QStringLiteral("group-") + QUuid::createUuid().toString(QUuid::Id128);
        started_ = false;
        control_.clear();
    }

    const bool dragon = line_.turn == 2;
    const auto persona = dragon ? QStringLiteral("dragon") : QStringLiteral("whale");
    const auto view = board.value("view").toObject();
    const bool matchingView = view.value("immersion").toBool() && view.value("persona").toString() == persona;
    if (!started_) {
        const auto now = QDateTime::currentMSecsSinceEpoch();
        if (!matchingView || (!control_.isEmpty() && view.value("control").toString() != control_)) {
            if (control_.isEmpty()) {
                control_ = QUuid::createUuid().toString(QUuid::Id128);
                navDeadline_ = now + 5000;
                lastNav_ = 0;
            }
            if (now >= navDeadline_) { cancel("board_view_timeout"); return; }
            if (now - lastNav_ >= 500 && display_->navigate(control_, dragon ? "immersion_dragon" : "immersion"))
                lastNav_ = now;
            result_ = "switching";
            return;
        }
        speech_->begin(mediaId_);
        speech_->answer(mediaId_, line_.text, true);
        started_ = true;
        result_ = "playing";
        return;
    }

    const auto speech = speech_->status();
    if (!matchingView || speech.value("active_id").toString() != mediaId_) { cancel("interrupted"); return; }
    const auto stage = speech.value("stage").toString();
    if (stage == "error") {
        receipts_.append(QJsonObject{{"turn", line_.turn}, {"persona", persona}, {"error", speech.value("last_error")}});
        cancel("speech_error");
        return;
    }
    if (stage != "complete") return;

    receipts_.append(QJsonObject{{"turn", line_.turn}, {"persona", persona}, {"voice", speech.value("voice")},
                                 {"segments", speech.value("completed_segments")},
                                 {"rendered_segments", speech.value("rendered_segments")},
                                 {"pcm_bytes", speech.value("pcm_bytes")}});
    mediaId_.clear();
    line_.text.clear();
    if (line_.turn == 3) {
        active_ = started_ = false;
        result_ = "completed";
        timer_.stop();
    }
}

QJsonObject DiscussionPlayback::status() const
{
    return {{"request_id", request_}, {"state", result_}, {"active", active_},
            {"turn", mediaId_.isEmpty() ? -1 : line_.turn}, {"receipts", receipts_}};
}
