#pragma once

#include "KnowledgeIndex.h"
#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

struct InferenceSettings {
    int contextTokens = 4096;
    int responseTokens = 256;
    double temperature = 0.2;
    int topK = 2;
    bool useKnowledge = true;
    bool chatMl = true;
    double repeatPenalty = 1.1;
    QString answerStyle = "Concise";
    QString instructions;
};

/** Persistent local stdio worker, with a compatible one-shot CLI fallback. */
class OfflineInference final : public QObject {
    Q_OBJECT
  public:
    explicit OfflineInference(QObject *parent = nullptr);
    ~OfflineInference() override;
    void setPaths(const QString &packageDirectory, const QString &executablePath);
    void ask(const QString &question, const QList<SourceHit> &sources, const QStringList &compactHistory,
             const InferenceSettings &settings);
    void stop();
    void setPaused(bool paused);
    bool supportsPause() const {
        return residentMode();
    }
    bool isBusy() const;
    bool isReady() const;
    void prepare(const QString &question, const QList<SourceHit> &sources, const QStringList &compactHistory,
                 const InferenceSettings &settings);
    qint64 runtimeProcessId() const {
        return process_.processId();
    }
    QString runtimeDescription() const {
        return runtimeDescription_;
    }

  signals:
    void textAvailable(const QString &text);
    void finished(const QString &error);
    void readinessChanged(bool ready);
    void contextPrepared();
    void runtimeError(const QString &error);
    void taskProgress(const QString &stage, int current, int total);
    void pauseChanged(bool paused);
    void responseMetrics(const QJsonObject &metrics);

  private:
    QString makePrompt(const QString &question, const QList<SourceHit> &sources, const QStringList &history,
                       bool chatMl, bool userHelp) const;
    void updateText();
    QStringList supportedFlags() const;
    bool residentMode() const;
    void startWorker();
    void flushRequests();
    void readWorkerEvents();
    QByteArray request(const char *type, const QString &question, const QList<SourceHit> &sources,
                       const QStringList &history, const InferenceSettings &settings, qint64 id) const;

    QProcess process_;
    QString packagePath_;
    QString executablePath_;
    QByteArray stdoutBuffer_;
    QString prompt_;
    bool stopped_ = false;
    mutable QString probedExecutable_;
    mutable QStringList cachedFlags_;
    QByteArray workerBuffer_;
    QByteArray queuedAsk_;
    QByteArray queuedPreparation_;
    QByteArray errorTail_;
    qint64 nextId_ = 0;
    qint64 activeId_ = 0;
    bool workerReady_ = false;
    bool preparing_ = false;
    bool busy_ = false;
    bool resetting_ = false;
    QString runtimeDescription_ = "Local runtime";
    bool cancelRequested_ = false;
};
