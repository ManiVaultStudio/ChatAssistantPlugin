#pragma once
#include "KnowledgeIndex.h"
#include <QDateTime>
#include <QJsonObject>
#include <QSqlDatabase>

struct ChatMessage {
    QString id, role, text;
    QList<SourceHit> sources;
    QDateTime created = QDateTime::currentDateTimeUtc();
    bool starred = false;
    bool edited = false;
};
struct Conversation {
    QString id, title = "New conversation", linkedContext;
    QList<ChatMessage> messages;
    bool pinned = false;
    int revision = 0;
    QString group;
};
struct ConversationSummary {
    QString id, title, preview;
    bool pinned = false;
    QString group;
};

class ChatStore {
  public:
    ChatStore();
    ~ChatStore();
    bool open(QString *error = nullptr);
    QList<ConversationSummary> list() const;
    Conversation load(const QString &id) const;
    bool save(Conversation &conversation, QString *error = nullptr);
    bool remove(const QString &id, QString *error = nullptr);
    QString path() const;
    static QString storageDirectory();
    static QString installationId(const QString &directory = {});
    bool importHistory(const QString &file, QString *error = nullptr);
    static QJsonObject toJson(const Conversation &chat);
    static Conversation fromJson(const QJsonObject &object);

  private:
    QSqlDatabase db_;
    QString connection_;
};
