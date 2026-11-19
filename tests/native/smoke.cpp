#include "KnowledgeIndex.h"
#include "OfflineInference.h"
#include <QGuiApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <QTextStream>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <functional>

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    if (argc == 4 && QString::fromLocal8Bit(argv[1]) == "--library") {
        KnowledgeIndex index;
        QString error;
        if (!index.open(QString::fromLocal8Bit(argv[2]) + "/knowledge.db", &error)) return 3;
        QFile file(QString::fromLocal8Bit(argv[3]));
        if (!file.open(QIODevice::ReadOnly)) return 11;
        const auto document = QJsonDocument::fromJson(file.readAll()).object();
        int total = 0, found = 0;
        std::function<void(const QJsonArray &)> inspect = [&](const QJsonArray &groups) {
            for (const auto &value : groups) {
                const auto group = value.toObject();
                for (const auto &question : group["questions"].toArray()) {
                    const auto prompt = question.isString() ? question.toString() : question.toObject()["prompt"].toString();
                    const auto result = index.retrieve(prompt, 2);
                    ++total;
                    if (result.error.isEmpty() && !result.hits.isEmpty()) ++found;
                    else QTextStream(stderr) << "No evidence: " << prompt << " " << result.error << '\n';
                }
                inspect(group["groups"].toArray());
            }
        };
        inspect(document["groups"].toArray());
        QTextStream(stdout) << "Library retrieval: " << found << '/' << total << " prompts have source excerpts\n";
        return total > 0 && found == total ? 0 : 12;
    }
    if (argc != 3) return 2;
    KnowledgeIndex index;
    QString error;
    if (!index.open(QString::fromLocal8Bit(argv[1]) + "/knowledge.db", &error)) return 3;
    const QString firstQuestion = "How do I select multiple cell types in the EvoViewer UI?";
    const QString secondQuestion = "How can I select more than one cell type in the EvoViewer UI?";
    InferenceSettings settings;
    settings.temperature = 0;
    settings.responseTokens = 160;
    const auto first = index.retrieve(firstQuestion, settings.topK);
    const auto second = index.retrieve(secondQuestion, settings.topK);
    if (first.hits.isEmpty() || second.hits.isEmpty()) return 4;
    OfflineInference inference;
    int stage = 0;
    qint64 originalPid = 0;
    qint64 firstTextMs = -1;
    QElapsedTimer timer;
    QString answer;
    bool pausedSeen = false, resumedSeen = false;
    QObject::connect(&inference, &OfflineInference::pauseChanged, &app, [&](bool paused) {
        if (stage != 3) return;
        if (paused) {
            pausedSeen = true;
            const auto frozen = answer;
            QTimer::singleShot(200, &app, [&, frozen] {
                if (answer != frozen || !inference.isBusy()) { app.exit(10); return; }
                inference.setPaused(false);
            });
        } else {
            resumedSeen = true;
            inference.stop();
        }
    });
    auto resetTimer = [&] { answer.clear(); firstTextMs = -1; timer.restart(); };
    QObject::connect(&inference, &OfflineInference::textAvailable, &app, [&](const QString& text) {
        answer = text;
        if (firstTextMs < 0 && !text.isEmpty()) firstTextMs = timer.elapsed();
    });
    QObject::connect(&inference, &OfflineInference::contextPrepared, &app, [&] {
        if (stage != 1 && stage != 4) return;
        const bool restarted = stage == 4;
        if (inference.runtimeProcessId() != originalPid) { app.exit(7); return; }
        stage = restarted ? 5 : 2;
        resetTimer();
        inference.ask(secondQuestion, second.hits, {}, settings);
    });
    QObject::connect(&inference, &OfflineInference::finished, &app, [&](const QString& failure) {
        if (stage == 3) {
            if (inference.isBusy() || !pausedSeen || !resumedSeen) { app.exit(8); return; }
            stage = 4;
            QTimer::singleShot(0, &app, [&] { inference.prepare(secondQuestion, second.hits, {}, settings); });
            return;
        }
        QTextStream(stdout) << "Stage " << stage << ": " << answer << '\n'
                            << "Backend: " << inference.runtimeDescription() << '\n'
                            << "First text: " << firstTextMs << " ms; total: " << timer.elapsed() << " ms\n";
        if (!failure.isEmpty() || !answer.contains("Shift", Qt::CaseInsensitive) || answer.contains("[end of text]")) {
            QTextStream(stderr) << failure; app.exit(5); return;
        }
        if (stage == 0) {
            originalPid = inference.runtimeProcessId();
            if (originalPid <= 0) { app.exit(9); return; }
            stage = 1;
            inference.prepare(secondQuestion, second.hits, {}, settings);
        } else if (stage == 2) {
            stage = 3;
            inference.ask(firstQuestion, first.hits, {}, settings);
            QTimer::singleShot(10, &app, [&] { inference.setPaused(true); });
        } else if (stage == 5) app.exit(0);
    });
    QTimer::singleShot(180000, &app, [&] { inference.stop(); app.exit(6); });
    timer.start();
    inference.setPaths(QString::fromLocal8Bit(argv[1]), QString::fromLocal8Bit(argv[2]));
    inference.ask(firstQuestion, first.hits, {}, settings); // Queue safely during initial loading.
    return app.exec();
}
