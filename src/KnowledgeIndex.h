#pragma once

#include <QList>
#include <QSqlDatabase>
#include <QString>

struct SourceHit {
    QString source;
    QString location;
    QString content;
    QString kind;
};

struct RetrievalResult {
    QList<SourceHit> hits;
    bool uiInstructionQuestion = false;
    QString error;
};

/** Read-only access to Local Knowledge Trainer's SQLite FTS5 schema v1. */
class KnowledgeIndex final {
  public:
    KnowledgeIndex();
    ~KnowledgeIndex();

    bool open(const QString &databasePath, QString *error);
    void close();
    bool isOpen() const;
    RetrievalResult retrieve(const QString &question, int limit) const;

    static bool isUserInterfaceQuestion(const QString &question);

  private:
    QStringList mentionedSources(const QString &question) const;
    QSqlDatabase database_;
    QString connectionName_;
};
