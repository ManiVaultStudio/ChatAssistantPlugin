#include "KnowledgeIndex.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUrl>
#include <QUuid>
#include <algorithm>

namespace {
QStringList keywords(const QString &question, bool uiMode) {
    static const QSet<QString> generalStop = {
        "a",    "an",    "and",   "are",  "as",   "at",    "be",    "by",    "can",   "does", "do",
        "for",  "from",  "how",   "in",   "into", "is",    "it",    "me",    "of",    "on",   "or",
        "our",  "the",   "their", "this", "to",   "what",  "when",  "where", "which", "who",  "why",
        "with", "would", "you",   "your", "file", "about", "using", "used",  "source"};
    static const QSet<QString> uiStop = {
        "change", "increase", "decrease", "adjust",   "modify",     "configure", "click", "set",
        "use",    "find",     "plugin",   "view",     "repository", "project",   "ui",    "interface",
        "app",    "software", "menu",     "settings", "show",       "hide",      "open"};
    const QRegularExpression rx(R"([\p{L}\p{N}_]+)");
    QStringList result;
    QString normalized = question.toLower();
    // Ordinary UI paraphrases should retrieve the same multi-selection guide.
    normalized.replace(QRegularExpression(R"(\bmore\s+than\s+one\b)"), "multiple");
    normalized.replace(QRegularExpression(R"(\bmulti[- ]?select(?:ion)?\b)"), "select multiple");
    auto it = rx.globalMatch(normalized);
    while (it.hasNext() && result.size() < 14) {
        const QString word = it.next().captured();
        if (word.size() < 2 || generalStop.contains(word) || (uiMode && uiStop.contains(word)) ||
            result.contains(word))
            continue;
        result << word;
    }
    return result;
}

bool isDocumentation(const SourceHit &hit) {
    if (hit.kind != "document")
        return false;
    const QString loc = hit.location.toLower();
    return loc.contains("readme") || loc.contains("docs/") || loc.contains("guide") || loc.endsWith(".md") ||
           loc.contains(".md:") || loc.contains(".pdf") || loc.contains(".txt");
}

bool hasInstruction(const QString &text) {
    static const QRegularExpression instructions(
        R"(\b(click|select|open|choose|menu|toolbar|slider|button|panel|tab|checkbox|dropdown|dialog|drag|toggle|navigate|adjust)\b)",
        QRegularExpression::CaseInsensitiveOption);
    return instructions.match(text).hasMatch();
}
} // namespace

KnowledgeIndex::KnowledgeIndex()
    : connectionName_("mentor-index-" + QUuid::createUuid().toString(QUuid::WithoutBraces)) {}

KnowledgeIndex::~KnowledgeIndex() {
    close();
}

bool KnowledgeIndex::open(const QString &databasePath, QString *error) {
    close();
    if (!QFileInfo::exists(databasePath)) {
        if (error)
            *error = "Knowledge database is missing: " + databasePath;
        return false;
    }
    // Open read-only: no user questions or source text are ever written to the training database.
    database_ = QSqlDatabase::addDatabase("QSQLITE", connectionName_);
    database_.setDatabaseName(databasePath);
    database_.setConnectOptions("QSQLITE_OPEN_READONLY");
    if (!database_.open()) {
        if (error)
            *error = database_.lastError().text();
        close();
        return false;
    }
    QSqlQuery check(database_);
    if (!check.exec("SELECT key, value FROM metadata WHERE key = 'schema_version'") || !check.next() ||
        check.value(1).toString() != "1") {
        if (error)
            *error = "Unsupported knowledge.db schema (expected Local Knowledge Trainer v1)";
        close();
        return false;
    }
    return true;
}

void KnowledgeIndex::close() {
    if (!QSqlDatabase::contains(connectionName_))
        return;
    if (database_.isValid())
        database_.close();
    database_ = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName_);
}

bool KnowledgeIndex::isOpen() const {
    return database_.isValid() && database_.isOpen();
}

bool KnowledgeIndex::isUserInterfaceQuestion(const QString &question) {
    static const QRegularExpression programmatic(
        R"(\b(implement|code|api|function|method|class|header|source)\b|c\+\+)",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression intent(
        R"(\b(how|where|steps?|can i)\b.*\b(change|adjust|increase|decrease|select|click|configure|enable|disable|show|hide|load|export|open|use|set|find)\b)",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression context(
        R"(\b(ui|interface|software|application|app|plugin|plot|scatterplot|panel|toolbar|menu|view|settings|size|opacity)\b)",
        QRegularExpression::CaseInsensitiveOption);
    return !programmatic.match(question).hasMatch() && intent.match(question).hasMatch() &&
           context.match(question).hasMatch();
}

QStringList KnowledgeIndex::mentionedSources(const QString &question) const {
    QSqlQuery q(database_);
    if (!q.exec("SELECT DISTINCT source FROM chunks"))
        return {};
    QStringList selected;
    while (q.next()) {
        const QString source = q.value(0).toString();
        const QUrl url(source);
        if (url.host().toLower() != "github.com")
            continue;
        const QStringList segments = url.path().split('/', Qt::SkipEmptyParts);
        if (segments.size() != 2)
            continue;
        QString repository = segments.last();
        if (repository.endsWith(".git"))
            repository.chop(4);
        if (repository.size() < 4)
            continue;
        const QRegularExpression boundary("(?<![\\w-])" + QRegularExpression::escape(repository) +
                                              "(?![\\w-])",
                                          QRegularExpression::CaseInsensitiveOption);
        if (boundary.match(question).hasMatch())
            selected.append(source);
    }
    return selected.mid(0, 4);
}

RetrievalResult KnowledgeIndex::retrieve(const QString &question, int limit) const {
    RetrievalResult result;
    result.uiInstructionQuestion = isUserInterfaceQuestion(question);
    if (!isOpen())
        return result;
    limit = qBound(1, limit, 8);
    const QStringList terms = keywords(question, result.uiInstructionQuestion);
    if (terms.isEmpty())
        return result;
    QStringList quoted;
    for (const QString &term : terms) {
        QString safe = term;
        safe.replace('"', "\"\"");
        quoted << '"' + safe + '"';
    }
    const QString queryText = quoted.join(" OR ");
    const QStringList scoped = mentionedSources(question);

    QString sql = "SELECT source, location, content, kind FROM chunks WHERE chunks MATCH ?";
    if (result.uiInstructionQuestion)
        sql += " AND kind = 'document'";
    if (!scoped.isEmpty()) {
        QStringList placeholders;
        for (qsizetype i = 0; i < scoped.size(); ++i)
            placeholders << "?";
        sql += " AND source IN (" + placeholders.join(',') + ')';
    }
    sql += " ORDER BY bm25(chunks) LIMIT ?";

    QSqlQuery query(database_);
    if (!query.prepare(sql)) {
        result.error = query.lastError().text();
        return result;
    }
    query.addBindValue(queryText);
    for (const QString &source : scoped)
        query.addBindValue(source);
    query.addBindValue(result.uiInstructionQuestion ? 80 : 60);
    if (!query.exec()) {
        result.error = query.lastError().text();
        return result;
    }
    QList<SourceHit> candidates;
    while (query.next()) {
        SourceHit hit{query.value(0).toString(), query.value(1).toString(), query.value(2).toString(),
                      query.value(3).toString()};
        if (!result.uiInstructionQuestion) {
            candidates.append(hit);
        } else if (isDocumentation(hit) && hasInstruction(hit.content)) {
            QStringList topics = terms;
            // Repository name is scope metadata, not evidence that a guide explains the requested action.
            if (!scoped.isEmpty()) {
                for (const QString &source : scoped) {
                    const QString repoName = QUrl(source).path().split('/', Qt::SkipEmptyParts).last();
                    topics.removeAll(repoName.toLower());
                }
            }
            int matched = 0;
            for (const QString &topic : topics)
                if (hit.content.contains(topic, Qt::CaseInsensitive))
                    ++matched;
            if (matched >= qMin(2, topics.size()) && !topics.isEmpty())
                candidates.append(hit);
        }
    }
    // For introductory requests, README/documentation is more useful than incidental code matches.
    const bool overview =
        question.contains(QRegularExpression(R"(\b(purpose|overview|summary|summarize|about|what is)\b)",
                                             QRegularExpression::CaseInsensitiveOption));
    if (overview && !result.uiInstructionQuestion) {
        std::stable_sort(candidates.begin(), candidates.end(), [](const SourceHit &a, const SourceHit &b) {
            const bool aReadme = a.location.contains("README", Qt::CaseInsensitive);
            const bool bReadme = b.location.contains("README", Qt::CaseInsensitive);
            if (aReadme != bReadme)
                return aReadme;
            return a.kind == "document" && b.kind != "document";
        });
    }
    result.hits = candidates.mid(0, limit);
    return result;
}
