#include "speech_bridge.h"
#include "display_bridge.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QUrl>
#include <QUuid>
#include <QtEndian>

namespace {
qint64 now(){return QDateTime::currentMSecsSinceEpoch();}
QString escaped(const QString& text){return QString::fromLatin1(QUrl::toPercentEncoding(text));}
QProcessEnvironment speechEnvironment(){
    auto env=QProcessEnvironment::systemEnvironment();
    for(const auto& key:env.keys())if(key.startsWith("ROBOMIND_AI_")||key.startsWith("ROBOMIND_LANCHAT_")||key.contains("API_KEY")||key.contains("TOKEN")||key.contains("PASSWORD")||key.contains("SECRET"))env.remove(key);
    return env;
}
QStringList subtitles(QString text){
    text.replace("```","");text.replace("**","");
    // Match the board's shipped font ranges; PC keeps the original complete answer.
    text.replace(QChar(0x201c),'"');text.replace(QChar(0x201d),'"');text.replace(QChar(0x2018),'\'');text.replace(QChar(0x2019),'\'');
    text.replace(QChar(0x2014),'-');text.replace(QChar(0x2013),'-');text.replace(QChar(0x2026),"...");
    QString supported;
    for(const char32_t cp:text.toUcs4()){
        if((cp>=0x20&&cp<=0x7e)||(cp>=0x3000&&cp<=0x301f)||(cp>=0x4e00&&cp<=0x9fff)||(cp>=0xff01&&cp<=0xff5e)||cp=='\n'||cp=='\t')supported+=QChar(static_cast<char16_t>(cp));
        else if(cp=='\r')continue;
        else supported+=' ';
    }
    text=supported;
    // Ceil of adv_w/16 from robomind_cjk_16.c, ASCII U+0020..U+007E.
    static constexpr int ascii[]={4,6,8,9,9,15,11,5,6,6,8,9,5,6,5,7,9,9,9,9,9,9,9,9,9,9,5,5,9,9,9,8,16,10,11,11,11,10,9,11,12,5,9,11,9,13,12,12,11,12,11,10,10,12,10,15,10,9,10,6,7,6,9,9,10,9,10,9,10,9,6,9,10,5,5,9,5,15,10,10,10,10,7,8,7,10,9,13,8,9,8,6,5,6,9};
    static_assert(sizeof(ascii)/sizeof(*ascii)==95,"Board ASCII font advances");
    QStringList result;QString part;int width=0,line=0,characters=0;
    const auto flush=[&]{if(!part.trimmed().isEmpty())result.append(part.trimmed());part.clear();width=line=characters=0;};
    for(QChar c:text){
        if(c=='\n'){if(characters>=16){flush();continue;}c=' ';}
        if(c=='\t')c=' ';
        const int advance=c.unicode()>=0x20&&c.unicode()<=0x7e?ascii[c.unicode()-0x20]:16;
        if(width+advance>280){if(line==0){part+='\n';line=1;width=0;}else flush();}
        part+=c;width+=advance;++characters;
        if(characters>=16&&QString("。！？!?").contains(c))flush();
    }
    if(!part.trimmed().isEmpty())result.append(part.trimmed());return result;
}
QByteArray wavPcm(const QByteArray& wav){
    if(wav.size()<44||wav.left(4)!="RIFF"||wav.mid(8,4)!="WAVE")return {};
    bool format=false;QByteArray pcm;
    for(qsizetype offset=12;offset+8<=wav.size();){const quint32 length=qFromLittleEndian<quint32>(wav.constData()+offset+4);if(length>static_cast<quint32>(wav.size()-offset-8))return {};
        if(wav.mid(offset,4)=="fmt "){if(length<16)return {};const auto* p=wav.constData()+offset+8;format=qFromLittleEndian<quint16>(p)==1&&qFromLittleEndian<quint16>(p+2)==1&&qFromLittleEndian<quint32>(p+4)==16000&&qFromLittleEndian<quint16>(p+14)==16;}
        if(wav.mid(offset,4)=="data")pcm=wav.mid(offset+8,length);offset+=8+length+(length&1);}
    return format&&!pcm.isEmpty()&&pcm.size()%2==0&&pcm.size()<=1280000?pcm:QByteArray{};
}
}
SpeechBridge::SpeechBridge(DisplayBridge* display,QObject* parent):QObject(parent),display_(display){
    QFile config(qEnvironmentVariable("ROBOMIND_TTS_CONFIG",QCoreApplication::applicationDirPath()+"/voice-backend.json"));
    if(config.open(QIODevice::ReadOnly)&&config.size()<16384){const auto data=QJsonDocument::fromJson(config.readAll()).object();python_=data.value("python").toString();worker_=data.value("worker").toString();runtime_=data.value("runtime").toString();
        localConfigured_=QFileInfo(python_).isAbsolute()&&QFile::exists(python_)&&QFileInfo(worker_).isAbsolute()&&QFile::exists(worker_)&&QFileInfo(runtime_).isAbsolute()&&QFile::exists(runtime_+"/voice.json");}
    provider_=qEnvironmentVariable("ROBOMIND_TTS_PROVIDER","auto");if(provider_=="auto")provider_=localConfigured_?"local_vits":"windows";
    timer_.setInterval(20);connect(&timer_,&QTimer::timeout,this,&SpeechBridge::pump);timer_.start();connect(display_,&DisplayBridge::audioStatus,this,&SpeechBridge::receive);
}
SpeechBridge::~SpeechBridge(){timer_.stop();for(auto process:{process_,local_})if(process){process->disconnect(this);process->kill();process->waitForFinished(1500);}}
void SpeechBridge::state(const QString& mode,const QString& text){if(!id_.isEmpty()){const bool sent=display_->sendMedia("ui/assistant_state",QString("id=%1;state=%2;text=%3").arg(id_,mode,escaped(text)),30000);if(mode=="thinking")announced_=sent;}}
void SpeechBridge::cancel(){
    if(!id_.isEmpty())display_->sendMedia("ui/audio_cancel","id="+id_,5000);
    if(local_&&stage_=="synthesizing"){local_->kill();local_=nullptr;}localRequest_.clear();
    id_.clear();sentences_.clear();pcm_.clear();stage_="idle";announced_=false;
    if(process_){process_->kill();process_=nullptr;}
}
void SpeechBridge::begin(const QString& id){
    cancel();const auto status=display_->status();
    if(!status.value("immersion_supported").toBool()||!status.value("view").toObject().value("immersion").toBool())return;
    id_=id;segment_=completed_=0;totalBytes_=0;lastError_.clear();touched_=now();stage_="thinking";state("thinking");
    if(!status.value("pcm_supported").toBool())fail("板端扬声器尚未就绪");
}
void SpeechBridge::answer(const QString& id,const QString& answer,bool success){
    if(id!=id_||id_.isEmpty()||stage_!="thinking")return;
    if(!success){state("error",subtitles(answer).value(0));stage_="error";lastError_="model_error";return;}
    sentences_=subtitles(answer);if(sentences_.isEmpty()||sentences_.size()>256){fail("字幕分句超过初版上限");return;}synthesize();
}
void SpeechBridge::fail(const QString& reason){lastError_=reason;if(!id_.isEmpty())display_->sendMedia("ui/audio_cancel","id="+id_,5000);state("error",reason);pcm_.clear();sentences_.clear();stage_="error";localRequest_.clear();if(local_){local_->kill();local_=nullptr;}if(process_){process_->kill();process_=nullptr;}}
void SpeechBridge::acceptPcm(QByteArray pcm){
    if(pcm.isEmpty()||pcm.size()%2||pcm.size()>1280000){fail("TTS 没有返回有效的 16kHz 单声道 PCM");return;}
    pcm_=std::move(pcm);sent_=received_=free_=chunk_=0;startAcknowledged_=false;stage_="starting";touched_=now();
}
void SpeechBridge::synthesizeLocal(){
    if(!localConfigured_){fail("未安装本地少女声线，请切换 Windows 中文语音");return;}
    stage_="synthesizing";touched_=now();localRequest_=QUuid::createUuid().toString(QUuid::Id128);
    const QString requestId=localRequest_;QString text=sentences_.first();text.remove('\n');
    const QByteArray request=QJsonDocument(QJsonObject{{"id",requestId},{"text",text}}).toJson(QJsonDocument::Compact)+'\n';
    if(local_&&local_->state()==QProcess::Running){local_->write(request);return;}
    auto* process=new QProcess(this);local_=process;localBuffer_.clear();auto env=speechEnvironment();env.insert("PYTHONUTF8","1");
    if(QDir(runtime_+"/tmp").exists()){env.insert("TEMP",runtime_+"/tmp");env.insert("TMP",runtime_+"/tmp");}process->setProcessEnvironment(env);process->setWorkingDirectory(runtime_);
    connect(process,&QProcess::started,this,[this,process,requestId,request]{if(local_==process&&localRequest_==requestId)process->write(request);});
    connect(process,&QProcess::readyReadStandardError,this,[process]{process->readAllStandardError();});
    connect(process,&QProcess::readyReadStandardOutput,this,[this,process]{
        if(local_!=process){process->readAllStandardOutput();return;}localBuffer_+=process->readAllStandardOutput();
        if(localBuffer_.size()>1800000){fail("本地语音输出超出范围");return;}
        for(qsizetype end;(end=localBuffer_.indexOf('\n'))>=0;){
            const QByteArray line=localBuffer_.left(end);localBuffer_.remove(0,end+1);const auto reply=QJsonDocument::fromJson(line).object();
            if(reply.value("event").toString()=="ready")continue;
            if(localRequest_.isEmpty()||stage_!="synthesizing"||reply.value("id").toString()!=localRequest_)continue;
            localRequest_.clear();if(!reply.value("error").toString().isEmpty()||reply.value("sample_rate").toInt()!=16000){fail("本地少女声线合成失败");return;}
            const auto decoded=QByteArray::fromBase64Encoding(reply.value("pcm").toString().toLatin1(),QByteArray::AbortOnBase64DecodingErrors);
            if(!decoded){fail("本地语音数据无效");return;}synthesisMs_=reply.value("synthesis_ms").toInt();acceptPcm(decoded.decoded);
        }
    });
    connect(process,QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),this,[this,process](int,QProcess::ExitStatus){process->deleteLater();if(local_==process){local_=nullptr;if(stage_=="synthesizing")fail("本地语音进程退出");}});
    connect(process,&QProcess::errorOccurred,this,[this,process](QProcess::ProcessError){if(local_==process){local_=nullptr;process->deleteLater();if(stage_=="synthesizing")fail("无法启动本地语音进程");}});
    process->start(python_,{"-u",worker_,"--runtime",runtime_});
}
void SpeechBridge::synthesize(){
    if(sentences_.isEmpty()){state("idle");stage_="complete";return;}
    if(provider_=="local_vits"){synthesizeLocal();return;}
    if(provider_!="windows"){fail("未知语音后端");return;}
#ifndef Q_OS_WIN
    fail("当前主机没有 Windows TTS");return;
#else
    if(!scratch_.isValid()){fail("无法创建语音临时目录");return;}
    const QString stem=QUuid::createUuid().toString(QUuid::Id128);
    const QString input=scratch_.filePath(stem+".txt"),output=scratch_.filePath(stem+".wav");QFile file(input);
    QString voiceText=sentences_.first();voiceText.remove('\n');
    if(!file.open(QIODevice::WriteOnly)||file.write(voiceText.toUtf8())<0){fail("无法准备语音文本");return;}file.close();QFile::remove(output);
    // Questions stay in an owned temporary file, outside process arguments and logs.
    const QString script=QString::fromLatin1(R"PS($ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Speech
$voice=New-Object System.Speech.Synthesis.SpeechSynthesizer
try {
 if(-not ($voice.GetInstalledVoices() | Where-Object { $_.Enabled -and $_.VoiceInfo.Culture.Name -eq 'zh-CN' })) { throw 'Chinese voice unavailable' }
 $voice.SelectVoiceByHints([System.Speech.Synthesis.VoiceGender]::NotSet,[System.Speech.Synthesis.VoiceAge]::NotSet,0,[System.Globalization.CultureInfo]::GetCultureInfo('zh-CN'))
 $format=New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(16000,[System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,[System.Speech.AudioFormat.AudioChannel]::Mono)
 $voice.SetOutputToWaveFile($env:ROBOMIND_TTS_OUTPUT,$format)
 $voice.Speak([IO.File]::ReadAllText($env:ROBOMIND_TTS_INPUT,[Text.Encoding]::UTF8))
 $voice.SetOutputToNull()
} finally { $voice.Dispose() }
)PS");
    QByteArray encoded;encoded.resize(script.size()*2);for(int i=0;i<script.size();++i)qToLittleEndian<quint16>(script[i].unicode(),reinterpret_cast<uchar*>(encoded.data()+i*2));
    auto* process=new QProcess(this);process_=process;auto env=speechEnvironment();
    env.insert("ROBOMIND_TTS_INPUT",input);env.insert("ROBOMIND_TTS_OUTPUT",output);process->setProcessEnvironment(env);
    const QString owner=id_;stage_="synthesizing";touched_=now();
    connect(process,QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),this,[this,process,owner,output,input](int code,QProcess::ExitStatus exit){
        process->deleteLater();QFile::remove(input);if(process_!=process||id_!=owner){QFile::remove(output);return;}process_=nullptr;
        if(code!=0||exit!=QProcess::NormalExit){fail("本机中文 TTS 合成失败");return;}
        QFile wave(output);if(!wave.open(QIODevice::ReadOnly)||wave.size()>1400000){fail("语音输出超出范围");return;}
        auto pcm=wavPcm(wave.readAll());wave.close();QFile::remove(output);acceptPcm(std::move(pcm));
    });
    connect(process,&QProcess::errorOccurred,this,[this,process,owner](QProcess::ProcessError){if(id_==owner&&process_==process){process_=nullptr;process->deleteLater();fail("无法启动本机 TTS");}});
    process->start(QDir::fromNativeSeparators(qEnvironmentVariable("SystemRoot","C:/Windows"))+"/System32/WindowsPowerShell/v1.0/powershell.exe",{"-NoProfile","-NonInteractive","-EncodedCommand",QString::fromLatin1(encoded.toBase64())});
#endif
}
void SpeechBridge::pump(){
    if(id_.isEmpty()||stage_=="idle"||stage_=="complete"||stage_=="error")return;
    const auto info=display_->status();if(!info.value("terminal_ready").toBool()||!info.value("view").toObject().value("immersion").toBool()){cancel();return;}
    if(now()-touched_>(stage_=="thinking"?150000:45000)){fail("语音播放或合成超时");return;}
    if(!announced_){state("thinking");return;} // Preserve thinking -> start order under TCP backpressure.
    if(stage_=="starting"){
        if(display_->sendMedia("ui/audio_start",QString("id=%1;seq=%2;bytes=%3;text=%4").arg(id_).arg(segment_).arg(pcm_.size()).arg(escaped(sentences_.first())),30000)){stage_="waiting_start";touched_=now();}
        return;
    }
    if(stage_!="streaming"||!startAcknowledged_)return;
    for(int batch=0;batch<8&&sent_<pcm_.size();++batch){const int count=qMin(768,static_cast<int>(pcm_.size())-sent_);
        if(free_-(sent_-received_)<count)return;
        if(!display_->sendMedia("ui/audio_chunk",QString("id=%1;segment=%2;seq=%3;data=%4").arg(id_).arg(segment_).arg(chunk_).arg(QString::fromLatin1(pcm_.mid(sent_,count).toBase64())),30000))return;
        sent_+=count;++chunk_;
    }
}
void SpeechBridge::receive(const QJsonObject& packet){
    if(id_.isEmpty()||packet.value("id").toString()!=id_||packet.value("segment").toInt(-1)!=segment_)return;
    if(packet.value("cancelled").toBool()){cancel();return;}
    const auto error=packet.value("error").toString();if(!error.isEmpty()){fail("板端语音反馈："+error);return;}
    const int received=packet.value("received").toInt(-1),available=packet.value("free").toInt(-1);
    if(received<0||received>sent_||available<0||available>65536)return;
    touched_=now();received_=received;free_=available;
    if(stage_=="waiting_start"){startAcknowledged_=true;stage_="streaming";}
    if(stage_=="streaming"&&packet.value("done").toBool()&&sent_==pcm_.size()&&packet.value("played").toInteger()==pcm_.size()){
        totalBytes_+=pcm_.size();++completed_;++segment_;sentences_.removeFirst();pcm_.clear();synthesize();
    }
}
QJsonObject SpeechBridge::status()const{return {{"stage",stage_},{"active_id",id_},{"segment",segment_},{"completed_segments",completed_},{"pcm_bytes",totalBytes_},{"last_error",lastError_},{"voice",provider_=="local_vits"?"派蒙风格少女（本地 VITS）":"Windows zh-CN"},{"provider",provider_},{"local_available",localConfigured_},{"synthesis_ms",synthesisMs_}};}
