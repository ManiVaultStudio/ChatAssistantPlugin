#include "ChatStore.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>

namespace {
QString uuid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
QJsonArray sourceJson(const QList<SourceHit> &sources) {
    QJsonArray array;
    for (const auto &s : sources)
        array.append(QJsonObject{
            {"source", s.source}, {"location", s.location}, {"content", s.content}, {"kind", s.kind}});
    return array;
}
QList<SourceHit> readSources(const QJsonArray &array) {
    QList<SourceHit> result;
    for (const auto &item : array) {
        const auto o = item.toObject();
        result.append({o["source"].toString(), o["location"].toString(), o["content"].toString(),
                       o["kind"].toString()});
    }
    return result;
}
} // namespace
ChatStore::ChatStore() : connection_("assistant-chats-" + uuid()) {}
ChatStore::~ChatStore() {
    if (db_.isValid())
        db_.close();
    db_ = QSqlDatabase();
    QSqlDatabase::removeDatabase(connection_);
}
QString ChatStore::installationId(const QString &directory) {
    const auto original = directory.isEmpty() ? QCoreApplication::applicationDirPath() : directory;
    auto identity = QFileInfo(original).canonicalFilePath();
    if (identity.isEmpty())
        identity = QFileInfo(original).absoluteFilePath();
    identity = QDir::cleanPath(QDir::fromNativeSeparators(identity));
#ifdef Q_OS_WIN
    identity = identity.toCaseFolded();
#endif
    return QString::fromLatin1(
        QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex().left(32));
}
QString ChatStore::storageDirectory() {
    const auto overridePath = qEnvironmentVariable("ASSISTANT_DATA_DIR");
    if (!overridePath.isEmpty())
        return QDir::cleanPath(overridePath);
    // Same per-user Qt convention as core's StandardPaths::getDownloadsDirectory.
    // The plugin owns only this child; no core setting or shared directory changes.
    const auto root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return root.isEmpty() ? QString()
                          : QDir(root).filePath("ManiVaultChatAssistant/instances/" + installationId());
}
QString ChatStore::path() const {
    if (db_.isOpen())
        return db_.databaseName();
    const auto base = storageDirectory();
    return base.isEmpty() ? QString() : QDir(base).filePath("chats.sqlite3");
}
bool ChatStore::open(QString *error) {
    if (db_.isOpen())
        return true;
    if (path().isEmpty()) {
        if (error)
            *error = "No writable per-user application data location was provided by the operating system.";
        return false;
    }
    if (!QDir().mkpath(QFileInfo(path()).absolutePath())) {
        if (error)
            *error = "Cannot create the chat history directory: " + QFileInfo(path()).absolutePath();
        return false;
    }
    if (!db_.isValid())
        db_ = QSqlDatabase::addDatabase("QSQLITE", connection_);
    db_.setDatabaseName(path());
    db_.setConnectOptions("QSQLITE_BUSY_TIMEOUT=3000");
    if (!db_.open()) {
        if (error)
            *error = db_.lastError().text();
        return false;
    }
    QSqlQuery q(db_);
    if (!q.exec("PRAGMA foreign_keys=ON")) {
        if (error)
            *error = q.lastError().text();
        db_.close();
        return false;
    }
    if (!q.exec("CREATE TABLE IF NOT EXISTS chats (id TEXT PRIMARY KEY,title TEXT,preview TEXT,pinned "
                "INTEGER,updated TEXT,revision INTEGER,payload BLOB)")) {
        if (error)
            *error = q.lastError().text();
        db_.close();
        return false;
    }
    if (!q.exec("CREATE INDEX IF NOT EXISTS chats_order ON chats(pinned DESC,updated DESC)") ||
        !q.exec("CREATE TABLE IF NOT EXISTS assistant_meta (key TEXT PRIMARY KEY,value TEXT)") ||
        !q.exec("CREATE TABLE IF NOT EXISTS chat_groups (chat_id TEXT PRIMARY KEY REFERENCES chats(id) ON "
                "DELETE CASCADE,name TEXT NOT NULL)") ||
        !q.exec("CREATE INDEX IF NOT EXISTS chat_groups_name ON chat_groups(name)")) {
        if (error)
            *error = q.lastError().text();
        db_.close();
        return false;
    }
    return true;
}
bool ChatStore::importHistory(const QString &legacyPath, QString *error) {
    if (!QFileInfo(legacyPath).isFile()) {
        if (error)
            *error = "Choose an existing assistant history database.";
        return false;
    }
    if (!open(error))
        return false;
    if (QFileInfo(legacyPath).canonicalFilePath() == QFileInfo(path()).canonicalFilePath()) {
        if (error)
            *error = "This is already the active history database.";
        return false;
    }
    QSqlQuery q(db_);
    const auto key =
        "imported/" +
        QString::fromLatin1(QCryptographicHash::hash(QFileInfo(legacyPath).canonicalFilePath().toUtf8(),
                                                     QCryptographicHash::Sha256)
                                .toHex());
    q.prepare("SELECT value FROM assistant_meta WHERE key=?");
    q.addBindValue(key);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    if (q.next())
        return true;
    q.finish();
    const QString legacyConnection = "assistant-legacy-" + uuid();
    bool success = false;
    {
        auto legacy = QSqlDatabase::addDatabase("QSQLITE", legacyConnection);
        legacy.setDatabaseName(legacyPath);
        legacy.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=3000");
        if (legacy.open() && db_.transaction()) {
            QSqlQuery read(legacy), insert(db_);
            success = read.exec("SELECT id,title,preview,pinned,updated,revision,payload FROM chats");
            insert.prepare("INSERT OR IGNORE INTO chats VALUES(?,?,?,?,?,?,?)");
            QSet<QString> importedIds;
            while (success && read.next()) {
                for (int i = 0; i < 7; ++i)
                    insert.bindValue(i, read.value(i));
                success = insert.exec();
                if (success && insert.numRowsAffected() > 0)
                    importedIds.insert(read.value(0).toString());
            }
            success = success && !read.lastError().isValid();
            if (success && legacy.tables().contains("chat_groups")) {
                QSqlQuery groups(legacy), addGroup(db_);
                success = groups.exec("SELECT chat_id,name FROM chat_groups");
                addGroup.prepare("INSERT OR IGNORE INTO chat_groups VALUES(?,?)");
                while (success && groups.next()) {
                    if (!importedIds.contains(groups.value(0).toString()))
                        continue;
                    addGroup.bindValue(0, groups.value(0));
                    addGroup.bindValue(1, groups.value(1));
                    success = addGroup.exec();
                }
                success = success && !groups.lastError().isValid();
            }
            if (success) {
                q.prepare("INSERT INTO assistant_meta VALUES(?,'1')");
                q.addBindValue(key);
                success = q.exec();
            }
            if (success)
                success = db_.commit();
            if (!success)
                db_.rollback();
        }
        legacy.close();
    }
    QSqlDatabase::removeDatabase(legacyConnection);
    if (!success) {
        if (error)
            *error =
                "Could not migrate existing history. The original database is unchanged at " + legacyPath;
        return false;
    }
    return true;
}
QList<ConversationSummary> ChatStore::list() const {
    QList<ConversationSummary> result;
    QSqlQuery q(db_);
    if (!db_.isOpen() ||
        !q.exec("SELECT c.id,c.title,c.preview,c.pinned,COALESCE(g.name,'') FROM chats c LEFT JOIN "
                "chat_groups g ON g.chat_id=c.id ORDER BY c.pinned DESC,c.updated DESC"))
        return result;
    while (q.next())
        result.append({q.value(0).toString(), q.value(1).toString(), q.value(2).toString(),
                       q.value(3).toBool(), q.value(4).toString()});
    return result;
}
Conversation ChatStore::load(const QString &id) const {
    QSqlQuery q(db_);
    q.prepare("SELECT c.payload,c.revision,COALESCE(g.name,'') FROM chats c LEFT JOIN chat_groups g ON "
              "g.chat_id=c.id WHERE c.id=?");
    q.addBindValue(id);
    if (!q.exec() || !q.next())
        return {};
    auto chat = fromJson(QJsonDocument::fromJson(qUncompress(q.value(0).toByteArray())).object());
    chat.revision = q.value(1).toInt();
    chat.group = q.value(2).toString();
    return chat;
}
bool ChatStore::save(Conversation &chat, QString *error) {
    if (!open(error))
        return false;
    if (chat.id.isEmpty())
        chat.id = uuid();
    chat.group = chat.group.simplified().left(80);
    if (!db_.transaction())
        return false;
    QSqlQuery check(db_);
    check.prepare("SELECT revision FROM chats WHERE id=?");
    check.addBindValue(chat.id);
    if (!check.exec()) {
        db_.rollback();
        return false;
    }
    if (check.next() && check.value(0).toInt() != chat.revision) {
        // Another view edited this chat. Preserve both versions as separate chats.
        chat.id = uuid();
        chat.title += " (copy)";
        chat.revision = 0;
    }
    ++chat.revision;
    QSqlQuery q(db_);
    q.prepare("INSERT OR REPLACE INTO chats VALUES(?,?,?,?,?,?,?)");
    q.addBindValue(chat.id);
    q.addBindValue(chat.title);
    q.addBindValue(chat.messages.isEmpty() ? QString() : chat.messages.last().text.left(180));
    q.addBindValue(chat.pinned);
    q.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    q.addBindValue(chat.revision);
    q.addBindValue(qCompress(QJsonDocument(toJson(chat)).toJson(QJsonDocument::Compact), 6));
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        --chat.revision;
        db_.rollback();
        return false;
    }
    if (!chat.group.isEmpty()) {
        QSqlQuery group(db_);
        group.prepare("INSERT INTO chat_groups VALUES(?,?)");
        group.addBindValue(chat.id);
        group.addBindValue(chat.group);
        if (!group.exec()) {
            if (error)
                *error = group.lastError().text();
            --chat.revision;
            db_.rollback();
            return false;
        }
    }
    if (!db_.commit()) {
        if (error)
            *error = db_.lastError().text();
        --chat.revision;
        db_.rollback();
        return false;
    }
    return true;
}
bool ChatStore::remove(const QString &id, QString *error) {
    QSqlQuery q(db_);
    q.prepare("DELETE FROM chats WHERE id=?");
    q.addBindValue(id);
    if (q.exec())
        return true;
    if (error)
        *error = q.lastError().text();
    return false;
}
QJsonObject ChatStore::toJson(const Conversation &chat) {
    QJsonArray messages;
    for (const auto &m : chat.messages)
        messages.append(QJsonObject{{"id", m.id},
                                    {"role", m.role},
                                    {"text", m.text},
                                    {"sources", sourceJson(m.sources)},
                                    {"created", m.created.toString(Qt::ISODateWithMs)},
                                    {"starred", m.starred},
                                    {"edited", m.edited}});
    return {{"version", 1},          {"id", chat.id},       {"title", chat.title},
            {"pinned", chat.pinned}, {"group", chat.group}, {"linkedContext", chat.linkedContext},
            {"messages", messages}};
}
Conversation ChatStore::fromJson(const QJsonObject &object) {
    Conversation chat;
    chat.id = object["id"].toString();
    chat.title = object["title"].toString("Imported chat");
    chat.pinned = object["pinned"].toBool();
    chat.group = object["group"].toString().simplified().left(80);
    chat.linkedContext = object["linkedContext"].toString();
    for (const auto &value : object["messages"].toArray()) {
        const auto o = value.toObject();
        const auto role = o["role"].toString();
        if (role != "user" && role != "assistant")
            continue;
        chat.messages.append({o["id"].toString(uuid()), role, o["text"].toString(),
                              readSources(o["sources"].toArray()),
                              QDateTime::fromString(o["created"].toString(), Qt::ISODateWithMs),
                              o["starred"].toBool(), o["edited"].toBool()});
    }
    return chat;
}
