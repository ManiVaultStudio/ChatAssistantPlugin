#include "RuntimePaths.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#ifndef ASSISTANT_DEV_PACKAGE_DIR
#define ASSISTANT_DEV_PACKAGE_DIR ""
#endif
#ifndef ASSISTANT_DEV_LLAMA_FILE
#define ASSISTANT_DEV_LLAMA_FILE ""
#endif
#ifndef ASSISTANT_DEV_LLAMA_BIN
#define ASSISTANT_DEV_LLAMA_BIN ""
#endif

namespace {
QStringList roots() {
    QStringList list;
    QDir current(QCoreApplication::applicationDirPath());
    for (int level = 0; level != 5; ++level) {
        list << current.absolutePath();
        if (!current.cdUp())
            break;
    }
    return list;
}

QString executableName() {
#ifdef Q_OS_WIN
    return "assistant-runtime.exe";
#else
    return "assistant-runtime";
#endif
}
} // namespace

QString RuntimePaths::modelFile(const QString &packageDirectory) {
    const QDir dir(packageDirectory);
    const QString single = dir.filePath("assistant.gguf");
    if (QFileInfo(single).isFile())
        return single;
    const QStringList shards =
        QDir(dir.filePath("model-shards")).entryList({"*.gguf"}, QDir::Files, QDir::Name);
    if (!shards.isEmpty())
        return dir.filePath("model-shards/" + shards.first());
    return {};
}

QString RuntimePaths::discoverPackage() {
    const QSettings settings("LocalKnowledgeAssistant", "KnowledgeAssistant");
    const QString saved = settings.value("packagePath").toString();
    if (QFileInfo(QDir(saved).filePath("knowledge.db")).isFile() && !modelFile(saved).isEmpty())
        return saved;
    const QStringList candidates = {
        QString::fromUtf8(ASSISTANT_DEV_PACKAGE_DIR),
        QDir(QCoreApplication::applicationDirPath()).filePath("assistant-package")};
    for (const QString &root : roots()) {
        for (const QString &rel : {"Plugins/ChatAssistantPlugin/data", "PlugIns/ChatAssistantPlugin/data",
                                   "ChatAssistantPlugin/data", "assistant-package"}) {
            const QString candidate = QDir(root).filePath(rel);
            if (QFileInfo(QDir(candidate).filePath("knowledge.db")).isFile() &&
                !modelFile(candidate).isEmpty())
                return candidate;
        }
    }
    for (const QString &candidate : candidates) {
        if (QFileInfo(QDir(candidate).filePath("knowledge.db")).isFile() && !modelFile(candidate).isEmpty())
            return candidate;
    }
    return {};
}

QString RuntimePaths::discoverExecutable() {
    const QSettings settings("LocalKnowledgeAssistant", "KnowledgeAssistant");
    const QString saved = settings.value("runtimePath").toString();
    if (QFileInfo(saved).isExecutable()) {
        // Upgrade a previously selected bundled CLI when its resident sibling exists.
        const auto sibling = QDir(QFileInfo(saved).absolutePath()).filePath(executableName());
        if (QFileInfo(saved).completeBaseName() == "llama-completion" && QFileInfo(sibling).isExecutable())
            return sibling;
        return saved;
    }
    const QString name = executableName();
    const QStringList candidates = {QString::fromUtf8(ASSISTANT_DEV_LLAMA_FILE),
                                    QDir(QString::fromUtf8(ASSISTANT_DEV_LLAMA_BIN)).filePath(name),
                                    QDir(QCoreApplication::applicationDirPath()).filePath(name)};
    for (const QString &root : roots()) {
        for (const QString &rel :
             {"Plugins/ChatAssistantPlugin/runtime/", "PlugIns/ChatAssistantPlugin/runtime/",
              "ChatAssistantPlugin/runtime/", "bin/", "bin/Release/"}) {
            const QString candidate = QDir(root).filePath(rel + name);
            if (QFileInfo(candidate).isExecutable())
                return candidate;
        }
    }
    for (const QString &candidate : candidates)
        if (QFileInfo(candidate).isExecutable())
            return candidate;
    const auto resident = QStandardPaths::findExecutable(name);
    if (!resident.isEmpty())
        return resident;
    return QStandardPaths::findExecutable("llama-completion");
}
