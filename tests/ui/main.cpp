#include <QtWidgets>
#include <QtTest>
#include "ChatPanel.h"
#include "ManiVaultControls.h"
#ifdef Q_OS_WIN
#include <windows.h>
#include <dbghelp.h>
LONG WINAPI traceFailure(EXCEPTION_POINTERS*) {
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    void* stack[64];
    auto count = CaptureStackBackTrace(0, 64, stack, nullptr);
    char buffer[sizeof(SYMBOL_INFO) + 512] = {};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = 511;
    for (unsigned i = 0; i < count; ++i) {
        DWORD64 offset;
        if (SymFromAddr(GetCurrentProcess(), reinterpret_cast<DWORD64>(stack[i]), &offset, symbol))
            fprintf(stderr, "%s + %llu\n", symbol->Name, offset);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

class ChatPanelTest {
public:
    static int run(const QString& output) {
        ChatPanel panel(nullptr, false);
        for (auto *shortcut : panel.findChildren<QShortcut *>(QString(), Qt::FindDirectChildrenOnly))
            if (shortcut->context() != Qt::WidgetWithChildrenShortcut) return 56;
        panel.resize(1280, 820);
        panel.chat_.title = "Exploring your knowledge";
        panel.title_->setText(panel.chat_.title);
        panel.chat_.messages = {
            {"one", "user", "How can I explore the documents in my knowledge library?"},
            {"two", "assistant", "## Explore your knowledge\nAsk a specific question about a topic in your documents.\n\n1. **Start with a topic** — name the tool or workflow.\n2. **Follow the evidence** — review the source excerpts below an answer.\n3. **Continue the conversation** — ask a follow-up in this chat.\n\n> Each conversation keeps its own context. You can explicitly link another chat from the conversation menu.\n\n| Task | Shortcut |\n| --- | --- |\n| Send a question | Enter |\n| Add a new line | Shift + Enter |"},
            {"three", "user", "Can I keep settings open while reading?"},
            {"four", "assistant", "**Yes.** Preferences open beside the conversation in a wide view, or in a separate nonmodal window in a narrow view. You can continue reading and typing."}
        };
        panel.renderMessages();
        panel.show();
        QTest::qWait(400);
        QDir().mkpath(output);
        QFile html(output + "/message.html");
        if (html.open(QIODevice::WriteOnly))
            for (auto* browser : panel.findChildren<ChatTextBrowser*>())
                html.write(browser->document()->toHtml().toUtf8());
        int checks = 0;
        for (const auto& name : {"Light", "Dark", "Midnight", "Nord", "Graphite", "Sepia", "Rose", "High contrast"}) {
            panel.themeChoice_->setCurrentText(name);
            panel.applyTheme();
            QTest::qWait(50);
            if (panel.palette().color(QPalette::Text) != panel.theme_.text) return 10;
            bool table = false;
            for (auto* browser : panel.findChildren<ChatTextBrowser*>())
                table |= browser->document()->toHtml().contains("<table");
            if (!table) return 14; // Theme refresh must preserve the original Markdown.
            panel.grab().save(output + "/" + QString(name).replace(' ', '-') + ".png");
            ++checks;
        }
        panel.themeChoice_->setCurrentText("Dark");
        panel.settingsButton_->setChecked(true);
        panel.toggleSettings(true);
        QTest::qWait(150);
        if (!panel.settingsPanel_->isVisible() || panel.chatColumn_->width() < 300) return 11;
        panel.grab().save(output + "/Settings-wide.png");
        panel.resize(580, 760);
        QTest::qWait(200);
        if (!panel.settingsDialog_ || panel.settingsDialog_->isModal() || !panel.input_->isEnabled()) return 12;
        panel.grab().save(output + "/Compact.png");
        panel.settingsDialog_->grab().save(output + "/Settings-floating.png");
        panel.toggleSettings(false);
        panel.input_->setPlainText("A line");
        QTest::keyClick(panel.input_, Qt::Key_Return, Qt::ShiftModifier);
        if (!panel.input_->toPlainText().contains('\n')) return 13;
        panel.input_->clear();
        // Editing stays reversible until submit, then branches without changing the source chat.
        panel.input_->setPlainText("My unsent draft");
        panel.editMessage(0);
        if (panel.editIndex_ != 0 || panel.input_->toPlainText() != panel.chat_.messages[0].text ||
            panel.cancelEditButton_->isHidden()) return 18;
        panel.cancelEdit();
        if (panel.input_->toPlainText() != "My unsent draft") return 19;
        const auto sourceId = panel.chat_.id;
        panel.saveChat();
        panel.editMessage(0);
        panel.input_->setPlainText("Revised question for the edited branch");
        panel.includeSources_->setChecked(false);
        AssistantControls::setEnabled(panel.sendButton_, true);
        panel.sendQuestion();
        if (panel.chat_.id == sourceId || panel.editIndex_ >= 0 || panel.chat_.messages.first().text != "Revised question for the edited branch") return 20;
        if (panel.store_.load(sourceId).messages.size() != 4) return 21;
        panel.switchChat(sourceId);
        if (panel.input_->toPlainText() != "My unsent draft") return 22;
        panel.showCommands();
        auto* palette = panel.findChild<QDialog*>("assistantCommands");
        if (!palette || palette->isModal()) return 23;
        auto* commandSearch = palette->findChild<QLineEdit*>("commandSearch");
        commandSearch->setText("Copy conversation");
        QTest::keyClick(commandSearch, Qt::Key_Return);
        if (!QApplication::clipboard()->text().contains("Exploring your knowledge")) return 24;
        panel.settingsSearch_->setText("temperature");
        bool answerGroup = false;
        for (auto* group : panel.settingsPanel_->findChildren<ChatGroup*>())
            if (group->header()->text() == "Answer preferences") answerGroup = !group->isHidden();
        if (!answerGroup) return 25;
        panel.settingsSearch_->clear();
        QStringList refreshLabels;
        for (int i = 0; i < panel.refreshRate_->count(); ++i) refreshLabels << panel.refreshRate_->itemText(i);
        panel.refreshRate_->setModel(new QStringListModel(refreshLabels, panel.refreshRate_));
        panel.refreshRate_->setCurrentIndex(3);
        if (panel.renderTimer_->interval() != 100) return 26;
        panel.loadSuggestions(":/assistant/suggested-questions.json");
        auto* questionSearch = panel.suggestionsCanvas_->findChild<QLineEdit*>("questionSearch");
        if (!questionSearch) return 27;
        questionSearch->setText("PathwayFinder");
        bool project = false, unrelated = false;
        for (auto* group : panel.suggestionsCanvas_->findChildren<ChatGroup*>()) {
            if (group->header()->text() == "PathwayFinder") project = !group->isHidden();
            if (group->header()->text() == "Data Properties") unrelated = !group->isHidden();
        }
        if (!project || unrelated) return 28;
        panel.resize(1280, 820);
        panel.sidebarButton_->setChecked(true);
        panel.sideTabs_->setCurrentIndex(1);
        QTest::qWait(100);
        panel.grab().save(output + "/Project-questions.png");
        questionSearch->clear();
        panel.chat_.messages.append({"chart-message", "assistant", "```chart\n{\"type\":\"bar\",\"labels\":[\"A\",\"B\"],\"values\":[2,4]}\n```"});
        panel.renderMessages();
        auto* chart = panel.messagesCanvas_->findChild<ChatChart*>();
        if (!chart || chart->contextMenuPolicy() != Qt::CustomContextMenu) return 40;
        auto* chartMenu = chart->parentWidget()->findChild<QMenu*>("messageContextMenu");
        bool chartCopy = false, messageDelete = false;
        for (auto* action : chartMenu->actions()) {
            if (action->text() == "Copy chart data (CSV)") { action->trigger(); chartCopy = true; }
            if (action->text() == "Delete this message") messageDelete = true;
        }
        if (!chartCopy || !messageDelete || !QApplication::clipboard()->text().contains("label,x,value")) return 41;
        panel.settingsButton_->setChecked(false);
        panel.resize(1100, 760);
        for (int i = 0; i < 16; ++i)
            panel.chat_.messages.append({QString::number(i + 10), "assistant", QString("A longer message for checking scrolling. ").repeated(12)});
        panel.waiting_ = true;
        panel.streamMessage_ = panel.chat_.messages.size() - 1;
        panel.followTail_ = false;
        panel.renderMessages();
        QTest::qWait(80);
        panel.scroll_->verticalScrollBar()->setValue(180);
        const int anchor = panel.scroll_->verticalScrollBar()->value();
        panel.replyText_ = QString("Streaming new content. ").repeated(60);
        panel.renderStreaming();
        QTest::qWait(80);
        if (panel.scroll_->verticalScrollBar()->value() != anchor) return 15;
        panel.finishReply({});
        QTest::qWait(100);
        if (panel.scroll_->verticalScrollBar()->value() != anchor) return 16;
        panel.branchChat(1);
        if (panel.chat_.messages.size() != 2) return 17;
        const auto deleteTarget = panel.chat_.id;
        panel.newChat();
        const auto keptChat = panel.chat_.id;
        auto confirmDelete = [] {
            for (auto* widget : QApplication::topLevelWidgets())
                if (auto* box = qobject_cast<QMessageBox*>(widget))
                    if (box->windowTitle() == "Delete conversation") box->button(QMessageBox::Yes)->click();
        };
        panel.waiting_ = true;
        QTimer::singleShot(20, qApp, confirmDelete);
        panel.deleteChat(deleteTarget);
        if (!panel.waiting_ || panel.chat_.id != keptChat || !panel.store_.load(deleteTarget).id.isEmpty()) return 42;
        panel.waiting_ = false;
        panel.refreshChats();
        const auto position = panel.chats_->visualItemRect(panel.chats_->currentItem()).center();
        panel.showChatMenu(position);
        auto* chatMenu = panel.chats_->findChild<QMenu*>("chatContextMenu");
        bool rename = false, remove = false;
        for (auto* action : chatMenu->actions()) {
            rename |= action->text() == "Rename…";
            remove |= action->text() == "Delete conversation…";
        }
        if (!rename || !remove) return 43;
        chatMenu->hide();
        QTimer::singleShot(20, qApp, confirmDelete);
        QTest::keyClick(panel.chats_, Qt::Key_Delete);
        if (panel.chat_.id == keptChat || !panel.store_.load(keptChat).id.isEmpty()) return 44;
        auto answerDialog = [](const QString &title, const QString &value) {
            for (auto *widget : QApplication::topLevelWidgets())
                if (auto *dialog = qobject_cast<QInputDialog *>(widget))
                    if (dialog->windowTitle() == title) { dialog->setTextValue(value); dialog->accept(); }
        };
        QTimer::singleShot(20, qApp, [answerDialog] { answerDialog("Move conversation to group", "Research"); });
        panel.groupChat();
        if (panel.chat_.group != "Research" || panel.store_.load(panel.chat_.id).group != "Research") return 45;
        panel.groupFilter_->setCurrentText("Group: Research");
        const auto groupedId = panel.chat_.id;
        panel.newChat();
        if (panel.chat_.group != "Research" || panel.groupFilter_->currentText() != "Group: Research") return 46;
        const auto activeId = panel.chat_.id;
        panel.waiting_ = true;
        QTimer::singleShot(20, qApp, [answerDialog] { answerDialog("Rename conversation", "Renamed without switching"); });
        panel.renameChat(groupedId);
        if (!panel.waiting_ || panel.chat_.id != activeId || panel.store_.load(groupedId).title != "Renamed without switching") return 47;
        panel.waiting_ = false;
        QTimer::singleShot(20, qApp, [&panel, answerDialog] {
            panel.chat_.messages.append({"late-reply", "assistant", "Response completed while rename was open"});
            panel.saveChat();
            answerDialog("Rename conversation", "Race-safe rename");
        });
        panel.renameChat();
        if (panel.chat_.messages.isEmpty() || panel.chat_.messages.last().id != "late-reply") return 48;
        panel.chat_.messages.last().sources = {{"tutorial", "test.md", "Evidence", "document"}};
        panel.saveChat();
        panel.editSavedMessage(0);
        auto *messageEditor = panel.findChild<QDialog *>("savedMessageEditor");
        if (!messageEditor || messageEditor->isModal()) return 49;
        messageEditor->findChild<QTextEdit *>("savedMessageText")->setPlainText("Manually corrected answer");
        messageEditor->findChild<QPushButton *>("saveMessageEdit")->click();
        if (panel.chat_.messages.first().text != "Manually corrected answer" || !panel.chat_.messages.first().edited || !panel.chat_.messages.first().sources.isEmpty()) return 50;
        const auto editedId = panel.chat_.id;
        panel.newChat();
        panel.switchChat(editedId);
        if (!panel.chat_.messages.first().edited || panel.chat_.group != "Research") return 51;
        panel.chatOrder_->setCurrentIndex(2);
        panel.sideTabs_->setCurrentIndex(0);
        panel.resize(1280, 820);
        QTest::qWait(80);
        panel.grab().save(output + "/Grouped-chats.png");
        panel.chat_.id.clear(); // Synthetic screenshot data must never become user history.
        QTextStream(stdout) << checks << " themes; Markdown; nonmodal settings; scoped shortcuts; scroll stability; branching; edit/cancel/resubmit; saved-message editing; drafts; grouping/filtering/sorting; rename races; command palette; preference search; native-model refresh rate; nested question search; context actions; Delete key; concurrent views passed\n";
        return 0;
    }
};
int main(int argc, char** argv) {
#ifdef Q_OS_WIN
    SetUnhandledExceptionFilter(traceFailure);
#endif
    QApplication app(argc, argv);
    QFontDatabase::addApplicationFont("C:/Windows/Fonts/segoeui.ttf");
    app.setFont(QFont("Segoe UI", 10));
    QTemporaryDir isolated;
    qputenv("ASSISTANT_DATA_DIR", isolated.path().toUtf8());
    qputenv("ASSISTANT_SETTINGS_FILE", (isolated.path() + "/settings.ini").toUtf8());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
    QCoreApplication::setOrganizationName("AssistantUiTest");
    QCoreApplication::setApplicationName("IsolatedUiTest");
    const auto oldPath = isolated.path() + "/old-history";
    const auto newPath = isolated.path() + "/new-history";
    qputenv("ASSISTANT_DATA_DIR", oldPath.toUtf8());
    {
        ChatStore old;
        Conversation chat;
        chat.id = "migration-test"; chat.title = "Preserved history"; chat.group = "Imported group";
        chat.messages.append({"migration-message", "user", "Keep this message"});
        QString error;
        if (!old.open(&error) || !old.save(chat, &error) || !old.save(chat, &error) || old.load(chat.id).group != "Imported group") return 30;
    }
    qputenv("ASSISTANT_DATA_DIR", newPath.toUtf8());
    {
        ChatStore migrated;
        QString error;
        if (!migrated.open(&error) || !migrated.list().isEmpty() || !migrated.importHistory(oldPath + "/chats.sqlite3", &error)) { QTextStream(stderr) << error; return 31; }
        if (migrated.load("migration-test").messages.size() != 1) return 32;
        if (migrated.load("migration-test").group != "Imported group") return 52;
        ChatStore otherView;
        if (!otherView.open()) return 53;
        auto first = migrated.load("migration-test"), second = otherView.load("migration-test");
        first.title = "Edited in first view";
        if (!migrated.save(first) || !otherView.save(second) || first.id == second.id || otherView.load(second.id).group != "Imported group") return 54;
        if (!otherView.remove(second.id)) return 55;
        if (!migrated.remove("migration-test")) return 33;
    }
    {
        ChatStore reopened;
        if (!reopened.open() || !reopened.importHistory(oldPath + "/chats.sqlite3") || !reopened.list().isEmpty()) return 34; // Deleted history must not reappear.
    }
    if (!QFileInfo::exists(oldPath + "/chats.sqlite3")) return 35;
    qunsetenv("ASSISTANT_DATA_DIR");
    QSettings globalSettings;
    globalSettings.setValue("GlobalSettings/Temporary/ApplicationTemporaryDirectory", QVariantMap{{"Value", isolated.path() + "/configured-temp"}});
    {
        ChatStore configured;
        const auto expected = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("ManiVaultChatAssistant/instances/" + ChatStore::installationId() + "/chats.sqlite3");
        if (configured.path() != expected || configured.path().contains("configured-temp")) return 36;
        if (ChatStore::installationId(isolated.path() + "/install-A") == ChatStore::installationId(isolated.path() + "/install-B")) return 37;
        if (ChatStore::installationId(isolated.path() + "/install-A") != ChatStore::installationId(isolated.path() + "/install-A/../install-A")) return 38;
    }
    qputenv("ASSISTANT_DATA_DIR", isolated.path().toUtf8());
    return ChatPanelTest::run(argc > 1 ? QString::fromLocal8Bit(argv[1]) : isolated.path());
}
