#pragma once
#include <QObject>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
class DisplayBridge;

// One board speaker, latest question owns it. The original PC reply is independent.
class SpeechBridge:public QObject {
    Q_OBJECT
public:
    explicit SpeechBridge(DisplayBridge* display,QObject* parent=nullptr);
    ~SpeechBridge() override;
    void begin(const QString& id);
    void answer(const QString& id,const QString& answer,bool success);
    void cancel();
    QJsonObject status() const;
private:
    void synthesize();
    void synthesizeLocal();
    void acceptPcm(QByteArray pcm);
    void pump();
    void receive(const QJsonObject& packet);
    void state(const QString& mode,const QString& text={});
    void fail(const QString& reason);
    DisplayBridge* display_;
    QString id_,lastError_,stage_="idle";
    QStringList sentences_;
    QPointer<QProcess> process_;
    QPointer<QProcess> local_;
    QString provider_,python_,worker_,runtime_,localRequest_;
    QByteArray localBuffer_;
    bool localConfigured_=false;
    int synthesisMs_=0;
    QTemporaryDir scratch_;
    QTimer timer_;
    QByteArray pcm_;
    int segment_=0,sent_=0,received_=0,free_=0,chunk_=0,completed_=0;
    qint64 touched_=0,totalBytes_=0;
    bool startAcknowledged_=false,announced_=false;
};
