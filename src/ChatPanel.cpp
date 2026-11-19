#include "ChatPanel.h"
#include "ManiVaultControls.h"
#include "RuntimePaths.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>
#include <QtWidgets>
#ifdef ASSISTANT_MANIVAULT_ACTIONS
#include <BackgroundTask.h>
#include <actions/GroupAction.h>
#include <actions/OptionsAction.h>
#include <actions/TriggerAction.h>
#endif

namespace {
class ReferenceBrowser final : public QTextBrowser {
  public:
    using QTextBrowser::QTextBrowser;

  protected:
    QVariant loadResource(int, const QUrl &) override { return {}; }
};

int streamRefreshMs(const QComboBox *control) {
    constexpr int intervals[]{16, 32, 64, 100};
    const int index = control->currentIndex();
    return index >= 0 && index < 4 ? intervals[index] : 32;
}
void fitQuestionButton(QPushButton *button) {
    const auto label = button->property("assistantQuestionLabel").toString();
    if (label.isEmpty())
        return;
    const auto text =
        button->fontMetrics().elidedText(label, Qt::ElideRight, std::max(24, button->width() - 48));
    button->setText(text);
    if (auto *action = AssistantControls::action(button))
        action->setText(text);
}
QSettings assistantSettings() {
    const auto path = qEnvironmentVariable("ASSISTANT_SETTINGS_FILE");
    if (!path.isEmpty())
        return QSettings(path, QSettings::IniFormat);
    const auto base = ChatStore::storageDirectory();
    if (!base.isEmpty())
        QDir().mkpath(base);
    return QSettings(base.isEmpty() ? ":/assistant/unavailable-settings.ini"
                                    : QDir(base).filePath("preferences.ini"),
                     QSettings::IniFormat);
}
QString newId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
QLabel *caption(const QString &text, QWidget *parent = nullptr) {
    auto *l = new QLabel(text, parent);
    l->setObjectName("muted");
    l->setWordWrap(true);
    return l;
}
QJsonObject defaultSuggestions() {
    QFile f(":/assistant/suggested-questions.json");
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}
bool validQuestionGroups(const QJsonArray &groups, int &groupCount, int &questionCount, int depth = 0) {
    if (depth > 5)
        return false;
    for (const auto &value : groups) {
        const auto group = value.toObject();
        if (!value.isObject() || ++groupCount > 100 || group["title"].toString().trimmed().isEmpty() ||
            group["title"].toString().size() > 160 ||
            (group.contains("groups") && !group["groups"].isArray()) ||
            (group.contains("questions") && !group["questions"].isArray()))
            return false;
        for (const auto &question : group["questions"].toArray()) {
            const auto prompt =
                question.isString() ? question.toString() : question.toObject()["prompt"].toString();
            if (++questionCount > 300 || prompt.trimmed().isEmpty() || prompt.size() > 5000 ||
                (!question.isString() && !question.isObject()))
                return false;
        }
        if (!validQuestionGroups(group["groups"].toArray(), groupCount, questionCount, depth + 1))
            return false;
    }
    return true;
}
QString plainChat(const Conversation &chat) {
    QString text = "# " + chat.title + "\n\n";
    for (const auto &m : chat.messages)
        text += "## " + (m.role == "user" ? QString("You") : QString("Assistant")) + "\n\n" + m.text + "\n\n";
    return text;
}
} // namespace

ChatPanel::ChatPanel(QWidget *parent, bool initializeRuntime) : QWidget(parent), inference_(this) {
    setObjectName("assistantRoot");
    setMinimumSize(420, 420);
    theme_ = ChatTheme::resolve("Light", {}, false);
    buildUi();
    buildSettings();
    loadSettings();
    loadingSettings_ = false;
#ifdef ASSISTANT_MANIVAULT_ACTIONS
    hostTask_ = new mv::BackgroundTask(this, "Knowledge Assistant", false, mv::Task::Status::Idle, true);
    connect(hostTask_, &mv::Task::requestAbort, this, [this] {
        if (waiting_) {
            stoppedReply_ = true;
            inference_.stop();
        }
    });
#endif
    applyTheme();
    if (saveHistory_->isChecked()) {
        QString error;
        if (!store_.open(&error))
            setStatus("History unavailable: " + error, false);
    }
    suggestions_ = defaultSuggestions();
    connect(sideTabs_, &QTabWidget::currentChanged, this, [this](int tab) {
        if (tab != 1 || suggestionsFile_)
            return;
        if (!suggestionsPath_.isEmpty())
            loadSuggestions(suggestionsPath_);
        if (!suggestionsFile_)
            refreshSuggestions();
    });
    refreshChats();
    if (!summaries_.isEmpty())
        switchChat(summaries_.first().id);
    else
        newChat();

    renderTimer_ = new QTimer(this);
    renderTimer_->setInterval(32);
    renderTimer_->setInterval(streamRefreshMs(refreshRate_));
    renderTimer_->setSingleShot(true);
    connect(renderTimer_, &QTimer::timeout, this, &ChatPanel::renderStreaming);
    connect(&inference_, &OfflineInference::textAvailable, this, [this](const QString &text) {
        if (!waiting_)
            return;
        replyText_ = text;
        if (firstTextMs_ < 0 && !text.isEmpty())
            firstTextMs_ = elapsed_.elapsed();
        if (!renderTimer_->isActive())
            renderTimer_->start();
    });
    connect(&inference_, &OfflineInference::finished, this, &ChatPanel::finishReply);
    connect(&inference_, &OfflineInference::readinessChanged, this, [this](bool ready) {
        if (!waiting_) {
            setStatus(ready ? "Ready · " + inference_.runtimeDescription() : "Preparing local assistant…",
                      ready);
            if (ready) {
                progressLabel_->setText("Ready");
                progress_->setRange(0, 100);
                progress_->setValue(0);
#ifdef ASSISTANT_MANIVAULT_ACTIONS
                if (hostTask_)
                    hostTask_->setFinished();
#endif
            }
        }
    });
    connect(&inference_, &OfflineInference::runtimeError, this, [this](const QString &error) {
        if (!waiting_)
            setStatus("Assistant unavailable · Check runtime settings", false);
        statusLabel_->setToolTip(error);
    });
    connect(&inference_, &OfflineInference::taskProgress, this, &ChatPanel::updateProgress);
    connect(&inference_, &OfflineInference::responseMetrics, this,
            [this](const QJsonObject &metrics) { metrics_ = metrics; });
    connect(&inference_, &OfflineInference::pauseChanged, this, [this](bool paused) {
        paused_ = paused;
        pauseButton_->setText(paused ? "Resume" : "Pause");
        pauseButton_->setProperty("iconName", paused ? "play" : "pause");
        pauseButton_->setIcon(chatIcon(paused ? "play" : "pause", theme_.text));
        if (auto *action = AssistantControls::action(pauseButton_)) {
            action->setText(pauseButton_->text());
            action->setIcon(pauseButton_->icon());
        }
        if (waiting_)
            updateProgress(paused ? "paused" : "generating");
    });
    prepareTimer_ = new QTimer(this);
    prepareTimer_->setInterval(300);
    prepareTimer_->setSingleShot(true);
    AssistantControls::onChanged(input_, this, [this] {
        prepareTimer_->stop();
        if (prepareWhileTyping_->isChecked() && !waiting_ && !inference_.isBusy() &&
            input_->toPlainText().trimmed().size() >= 6)
            prepareTimer_->start();
        input_->setFixedHeight(std::clamp(int(input_->document()->size().height()) + 16, 54, 140));
    });
    connect(prepareTimer_, &QTimer::timeout, this, [this] {
        if (waiting_ || inference_.isBusy())
            return;
        const auto question = input_->toPlainText().trimmed();
        if (question.size() < 6)
            return;
        const auto settings = currentSettings();
        inference_.prepare(question, retrieveSources(question, settings).hits, history(), settings);
    });
    auto *clock = new QTimer(this);
    clock->setInterval(150);
    connect(clock, &QTimer::timeout, this, [this] {
        if (waiting_)
            statisticsLabel_->setText(
                QString("%1 s%2")
                    .arg(elapsed_.elapsed() / 1000.0, 0, 'f', 1)
                    .arg(firstTextMs_ >= 0 ? QString(" · first text %1 ms").arg(firstTextMs_) : QString()));
    });
    clock->start();
    auto *refresh = new QTimer(this);
    refresh->setInterval(5000);
    connect(refresh, &QTimer::timeout, this, [this] {
        if (sidebar_->isVisible() && sideTabs_->currentIndex() == 0 && !waiting_ &&
            saveHistory_->isChecked())
            refreshChats();
    });
    refresh->start();
    AssistantControls::setEnabled(sendButton_, false);
    if (initializeRuntime) {
        setStatus("Starting local assistant…", false);
        QTimer::singleShot(0, this, &ChatPanel::refreshPaths);
    } else {
        setStatus("Layout preview · No model loaded", false);
    }
}
ChatPanel::~ChatPanel() {
    cancelForNavigation();
    saveChat();
}

QPushButton *ChatPanel::tool(const QString &icon, const QString &title, const QString &tip, QWidget *parent) {
    const bool toggle = tip == "Show or hide chats and the question library" ||
                        tip == "Appearance, answers, runtime and privacy";
    auto *b = AssistantControls::trigger(this, title.isEmpty() ? tip : title, parent ? parent : this, toggle,
                                         title.isEmpty());
    b->setProperty("iconName", icon);
    b->setIcon(chatIcon(icon, theme_.text));
    if (auto *a = AssistantControls::action(b))
        a->setIcon(b->icon());
    b->setIconSize({18, 18});
    b->setCursor(Qt::PointingHandCursor);
    b->setAccessibleName(title.isEmpty() ? tip : title);
    chatTip(b, title.isEmpty() ? tip : title, tip);
    return b;
}

void ChatPanel::buildUi() {
    auto *root = new AssistantControls::VBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);
    auto *toolbar = new QFrame(this);
    toolbar->setObjectName("toolbar");
    auto *top = new AssistantControls::HBoxLayout(toolbar);
    top->setContentsMargins(12, 9, 12, 9);
    top->setSpacing(10);
    sidebarButton_ = tool("sidebar", "", "Show or hide chats and the question library", toolbar);
    sidebarButton_->setCheckable(true);
    sidebarButton_->setChecked(true);
    top->addWidget(sidebarButton_);
    auto *brand = new QLabel(toolbar);
    brand->setPixmap(chatIcon("chat", theme_.accent).pixmap(24, 24));
    brand->setObjectName("brandIcon");
    top->addWidget(brand);
    auto *identity = new AssistantControls::VBoxLayout;
    identity->setSpacing(2);
    title_ = new QLabel("Knowledge Assistant", toolbar);
    title_->setObjectName("heading");
    title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    identity->addWidget(title_);
    statusLabel_ = caption("Preparing local assistant…", toolbar);
    statusLabel_->setObjectName("runtimeStatus");
    statusLabel_->setWordWrap(false);
    statusLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    identity->addWidget(statusLabel_);
    top->addLayout(identity, 1);
    auto *create = tool("plus", "New chat", "Start an independent conversation", toolbar);
    create->setObjectName("newChatButton");
    AssistantControls::onTrigger(create, this, [this] { newChat(); });
    top->addWidget(create);
    commands_.append(AssistantControls::action(create));
    auto *paletteButton = tool("search", "", "Search commands and keyboard shortcuts", toolbar);
    paletteButton->setObjectName("commandsButton");
    chatTip(paletteButton, "Command palette", "Find conversation actions without navigating menus.",
            "Ctrl/Cmd+K");
    AssistantControls::onTrigger(paletteButton, this, [this] { showCommands(); });
    top->addWidget(paletteButton);
    auto *more = tool("down", "", "Conversation actions", toolbar);
    auto *menu = new QMenu(more);
    auto add = [&](const QString &name, const QString &icon, auto callback) {
        auto *a = AssistantControls::command(menu, name, chatIcon(icon, theme_.text));
        commands_.append(a);
        connect(a, &QAction::triggered, this, callback);
    };
    add("Rename conversation…", "file", [this] { renameChat(); });
    add("Move conversation to a group…", "file", [this] { groupChat(); });
    add("Pin / unpin conversation", "star", [this] {
        chat_.pinned = !chat_.pinned;
        saveChat();
        refreshChats();
    });
    add("Branch conversation", "branch", [this] { branchChat(chat_.messages.size() - 1); });
    add("Include context from another chat…", "chat", [this] { useOtherContext(); });
    menu->addSeparator();
    add("Copy conversation", "copy", [this] { copyChat(); });
    add("Export conversation…", "download", [this] { exportChat(); });
    add("Import conversation…", "file", [this] { importChat(); });
    add("Copy runtime diagnostics", "chart", [this] { copyDiagnostics(); });
    menu->addSeparator();
    add("Delete this conversation…", "trash", [this] { deleteChat(); });
    add("Delete all saved conversations…", "trash", [this] { deleteAllChats(); });
    more->setMenu(menu);
    top->addWidget(more);
    settingsButton_ = tool("settings", "", "Appearance, answers, runtime and privacy", toolbar);
    settingsButton_->setObjectName("settingsButton");
    settingsButton_->setCheckable(true);
    top->addWidget(settingsButton_);
    commands_.append(AssistantControls::action(settingsButton_));
    commands_.append(AssistantControls::action(sidebarButton_));
    AssistantControls::onToggle(settingsButton_, this, [this](bool show) { toggleSettings(show); });
    root->addWidget(toolbar);

    splitter_ = new QSplitter(Qt::Horizontal, this);
    splitter_->setChildrenCollapsible(false);
    root->addWidget(splitter_, 1);
    sidebar_ = new QFrame(splitter_);
    sidebar_->setObjectName("sidebar");
    sidebar_->setMinimumWidth(180);
    sidebar_->setMaximumWidth(330);
    auto *side = new AssistantControls::VBoxLayout(sidebar_);
    side->setContentsMargins(10, 10, 10, 10);
    side->setSpacing(10);
    sideTabs_ = new QTabWidget(sidebar_);
    sideTabs_->setObjectName("sideTabs");
    side->addWidget(sideTabs_);
    auto *chatsPage = new QWidget;
    auto *cl = new AssistantControls::VBoxLayout(chatsPage);
    cl->setContentsMargins(0, 10, 0, 0);
    chatSearch_ = AssistantControls::line(this, "Search conversations", chatsPage);
    chatSearch_->setPlaceholderText("Search conversations");
    chatSearch_->setClearButtonEnabled(true);
    cl->addWidget(chatSearch_);
    groupFilter_ = AssistantControls::option(this, "Conversation group",
                                             {"All groups", "Pinned only", "Ungrouped"}, chatsPage);
    groupFilter_->setObjectName("groupFilter");
    groupFilter_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    cl->addWidget(groupFilter_);
    chatTip(groupFilter_, "Filter conversations",
            "Show a saved group, pinned chats, or ungrouped chats. Use a chat's right-click menu to create "
            "or change its group.");
    chatOrder_ = AssistantControls::option(this, "Chat order", {"Recent first", "Title (A–Z)", "Group (A–Z)"},
                                           chatsPage);
    chatOrder_->setObjectName("chatOrder");
    chatOrder_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    cl->addWidget(chatOrder_);
    chatTip(chatOrder_, "Conversation order",
            "Pinned chats stay first. Sort the remaining chats by recent activity, title or group.");
    for (auto *control : {groupFilter_, chatOrder_})
        AssistantControls::onChanged(control, this, [this] {
            if (!loadingSettings_ && !refreshingChats_) {
                saveSettings();
                refreshChats();
            }
        });
    chats_ = new QListWidget;
    chats_->setObjectName("chatList");
    chats_->setSpacing(4);
    chats_->setWordWrap(true);
    chats_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    chats_->setTextElideMode(Qt::ElideRight);
    chats_->installEventFilter(this);
    chats_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(chats_, &QListWidget::customContextMenuRequested, this, &ChatPanel::showChatMenu);
    chatTip(chats_, "Saved conversations",
            "Select a conversation to open it. Right-click for actions; Delete removes the selected "
            "conversation after confirmation.",
            "Delete");
    cl->addWidget(chats_, 1);
    sideTabs_->addTab(chatsPage, "Chats");
    auto *promptScroll = new QScrollArea;
    promptScroll->setWidgetResizable(true);
    promptScroll->setFrameShape(QFrame::NoFrame);
    suggestionsCanvas_ = new QWidget;
    suggestionsLayout_ = new AssistantControls::VBoxLayout(suggestionsCanvas_);
    suggestionsLayout_->setContentsMargins(0, 10, 0, 0);
    suggestionsLayout_->setSpacing(10);
    promptScroll->setWidget(suggestionsCanvas_);
    sideTabs_->addTab(promptScroll, "Questions");
    sideTabs_->setTabToolTip(0, "Browse, search and organize your conversations");
    sideTabs_->setTabToolTip(1, "Browse documented workflows or import a question library");
    connect(chatSearch_, &QLineEdit::textChanged, this, &ChatPanel::refreshChats);
    connect(chats_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        if (item && !loadingSettings_ && item->data(Qt::UserRole).toString() != chat_.id)
            switchChat(item->data(Qt::UserRole).toString());
    });
    chatTip(chatSearch_, "Search chats", "Filter saved conversations by title or preview.");
    AssistantControls::onToggle(sidebarButton_, this, [this](bool show) {
        AssistantControls::setVisible(sidebar_, show);
        saveSettings();
    });

    chatColumn_ = new QWidget(splitter_);
    chatColumn_->setObjectName("chatColumn");
    chatColumn_->setMinimumWidth(240);
    auto *column = new AssistantControls::VBoxLayout(chatColumn_);
    column->setContentsMargins(8, 0, 8, 0);
    column->setSpacing(6);
    auto *searchRow = new AssistantControls::HBoxLayout;
    messageSearch_ = AssistantControls::line(this, "Find in conversation", chatColumn_);
    messageSearch_->setPlaceholderText("Find in this conversation");
    messageSearch_->setClearButtonEnabled(true);
    messageSearch_->setObjectName("messageSearch");
    searchRow->addWidget(messageSearch_, 1);
    contextLabel_ = caption("");
    contextLabel_->setObjectName("contextChip");
    searchRow->addWidget(contextLabel_);
    column->addLayout(searchRow);
    connect(messageSearch_, &QLineEdit::textChanged, this, [this](const QString &term) {
        if (!term.isEmpty())
            followTail_ = false;
        for (auto *row : messagesCanvas_->findChildren<QWidget *>("messageRow", Qt::FindDirectChildrenOnly))
            row->setVisible(term.isEmpty() ||
                            row->property("messageText").toString().contains(term, Qt::CaseInsensitive));
    });
    scroll_ = new QScrollArea;
    scroll_->setObjectName("chatScroll");
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    messagesCanvas_ = new QWidget;
    messagesCanvas_->setObjectName("messageCanvas");
    messagesLayout_ = new AssistantControls::VBoxLayout(messagesCanvas_);
    messagesLayout_->setContentsMargins(4, 14, 4, 14);
    messagesLayout_->setSpacing(12);
    messagesLayout_->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    scroll_->setWidget(messagesCanvas_);
    column->addWidget(scroll_, 1);
    scroll_->viewport()->installEventFilter(this);
    scroll_->verticalScrollBar()->installEventFilter(this);
    connect(scroll_->verticalScrollBar(), &QScrollBar::rangeChanged, this, [this] {
        if (followTail_ && autoScroll_ && autoScroll_->isChecked())
            QTimer::singleShot(0, this, &ChatPanel::followLatest);
    });
    connect(scroll_->verticalScrollBar(), &QScrollBar::sliderPressed, this, [this] {
        followTail_ = false;
        AssistantControls::setVisible(latestButton_, true);
    });
    connect(scroll_->verticalScrollBar(), &QScrollBar::sliderReleased, this, [this] {
        auto *s = scroll_->verticalScrollBar();
        followTail_ = s->maximum() - s->value() < 24;
        AssistantControls::setVisible(latestButton_, !followTail_);
    });
    latestButton_ = tool("down", "Jump to latest", "Resume following new replies", chatColumn_);
    latestButton_->setObjectName("latestButton");
    AssistantControls::setVisible(latestButton_, false);
    column->addWidget(latestButton_, 0, Qt::AlignHCenter);
    AssistantControls::onTrigger(latestButton_, this, [this] {
        followTail_ = true;
        followLatest();
        AssistantControls::setVisible(latestButton_, false);
    });

    auto *task = new QFrame;
    task->setObjectName("taskStrip");
    auto *taskLayout = new AssistantControls::VBoxLayout(task);
    taskLayout->setContentsMargins(10, 4, 10, 4);
    taskLayout->setSpacing(3);
    auto *labels = new AssistantControls::HBoxLayout;
    progressLabel_ = caption("Ready");
    statisticsLabel_ = caption("");
    for (auto *label : {progressLabel_, statisticsLabel_}) {
        label->setWordWrap(false);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        label->setFixedHeight(label->fontMetrics().height() + 4);
    }
    statisticsLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    statisticsLabel_->setMinimumWidth(125);
    labels->addWidget(progressLabel_, 1);
    labels->addWidget(statisticsLabel_);
    taskLayout->addLayout(labels);
    progress_ = new QProgressBar;
    progress_->setObjectName("taskProgress");
    progress_->setFixedHeight(3);
    progress_->setTextVisible(false);
    progress_->setRange(0, 100);
    progress_->setValue(0);
    taskLayout->addWidget(progress_);
    column->addWidget(task);
    auto *composer = new QFrame;
    composer->setObjectName("composer");
    auto *compose = new AssistantControls::VBoxLayout(composer);
    compose->setContentsMargins(12, 8, 12, 8);
    compose->setSpacing(4);
    input_ = AssistantControls::text(this, "Message", composer);
    input_->setObjectName("chatInput");
    input_->setAcceptRichText(false);
    input_->setPlaceholderText("Ask about your local knowledge…");
    input_->setFixedHeight(60);
    input_->installEventFilter(this);
    compose->addWidget(input_);
    auto *composerActions = new AssistantControls::HBoxLayout;
    auto *questions = tool("chat", "Questions", "Open the collapsible question library", composer);
    composerActions->addWidget(questions);
    AssistantControls::onTrigger(questions, this, [this] {
        sidebarButton_->setChecked(true);
        sideTabs_->setCurrentIndex(1);
    });
    composerHint_ = caption("Enter to send · Shift+Enter for a new line");
    composerHint_->setWordWrap(false);
    composerHint_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    composerActions->addWidget(composerHint_, 1);
    cancelEditButton_ =
        tool("close", "Cancel edit", "Discard this edit and restore your unsent draft", composer);
    cancelEditButton_->setObjectName("cancelEditButton");
    composerActions->addWidget(cancelEditButton_);
    AssistantControls::setVisible(cancelEditButton_, false);
    AssistantControls::onTrigger(cancelEditButton_, this, [this] { cancelEdit(); });
    pauseButton_ = tool("pause", "Pause", "Pause generation without discarding the response", composer);
    pauseButton_->setObjectName("pauseButton");
    AssistantControls::setVisible(pauseButton_, false);
    composerActions->addWidget(pauseButton_);
    AssistantControls::onTrigger(pauseButton_, this, [this] { inference_.setPaused(!paused_); });
    stopButton_ = tool("stop", "Stop", "Stop the response while keeping the model loaded", composer);
    stopButton_->setObjectName("stopButton");
    AssistantControls::setVisible(stopButton_, false);
    composerActions->addWidget(stopButton_);
    AssistantControls::onTrigger(stopButton_, this, [this] {
        stoppedReply_ = true;
        inference_.stop();
        updateProgress("stopping");
    });
    sendButton_ = tool("send", "Send", "Send your message", composer);
    sendButton_->setObjectName("sendButton");
    composerActions->addWidget(sendButton_);
    AssistantControls::onTrigger(sendButton_, this, [this] { sendQuestion(); });
    compose->addLayout(composerActions);
    column->addWidget(composer);
    settingsPanel_ = new QFrame(splitter_);
    settingsPanel_->setObjectName("settingsPane");
    settingsPanel_->setMinimumWidth(280);
    settingsPanel_->setMaximumWidth(380);
    AssistantControls::setVisible(settingsPanel_, false);
    splitter_->setStretchFactor(0, 0);
    splitter_->setStretchFactor(1, 1);
    splitter_->setStretchFactor(2, 0);
    splitter_->setSizes({235, 780, 340});
    auto *fresh = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N), this);
    connect(fresh, &QShortcut::activated, this, &ChatPanel::newChat);
    auto *find = new QShortcut(QKeySequence::Find, this);
    connect(find, &QShortcut::activated, this, [this] {
        messageSearch_->setFocus();
        messageSearch_->selectAll();
    });
    auto *commands = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_K), this);
    connect(commands, &QShortcut::activated, this, &ChatPanel::showCommands);
    auto *preferences = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma), this);
    connect(preferences, &QShortcut::activated, this,
            [this] { settingsButton_->setChecked(!settingsButton_->isChecked()); });
    auto *pause = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Space), this);
    connect(pause, &QShortcut::activated, this, [this] {
        if (waiting_ && inference_.supportsPause())
            inference_.setPaused(!paused_);
    });
    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escape, &QShortcut::activated, this, [this] {
        if (waiting_) {
            stoppedReply_ = true;
            inference_.stop();
        } else if (settingsButton_->isChecked())
            settingsButton_->setChecked(false);
    });
    for (auto *shortcut : findChildren<QShortcut *>(QString(), Qt::FindDirectChildrenOnly))
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
}

void ChatPanel::buildSettings() {
    QObject *actionOwner = this;
    auto *frame = new AssistantControls::VBoxLayout(settingsPanel_);
    frame->setContentsMargins(12, 10, 12, 10);
    auto *heading = new AssistantControls::HBoxLayout;
    auto *text = new QLabel("Preferences");
    text->setObjectName("sectionTitle");
    heading->addWidget(text, 1);
    auto *close = tool("close", "", "Close preferences", settingsPanel_);
    AssistantControls::onTrigger(close, this, [this] { settingsButton_->setChecked(false); });
    heading->addWidget(close);
    frame->addLayout(heading);
    settingsSearch_ = AssistantControls::line(this, "Search preferences", settingsPanel_);
    settingsSearch_->setObjectName("settingsSearch");
    settingsSearch_->setPlaceholderText("Search preferences…");
    settingsSearch_->setClearButtonEnabled(true);
    chatTip(
        settingsSearch_, "Search preferences",
        "Find a group by its setting names or help text. Clear the search to restore your expanded groups.");
    frame->addWidget(settingsSearch_);
    settingsScroll_ = new QScrollArea;
    settingsScroll_->setWidgetResizable(true);
    settingsScroll_->setFrameShape(QFrame::NoFrame);
    settingsScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *contents = new QWidget;
    auto *layout = new AssistantControls::VBoxLayout(contents);
    layout->setContentsMargins(0, 0, 4, 0);
    layout->setSpacing(12);
    auto group = [&](const QString &title) {
        auto *g = new ChatGroup(title);
        g->setExpanded(true);
#ifdef ASSISTANT_MANIVAULT_ACTIONS
        auto *actionGroup = new mv::gui::GroupAction(this, title, true);
        actionOwner = actionGroup;
        connect(g, &QObject::destroyed, actionGroup, &QObject::deleteLater);
        connect(g->header(), &QToolButton::toggled, actionGroup, &mv::gui::GroupAction::setExpanded);
        connect(actionGroup, &mv::gui::GroupAction::expanded, g, [g] {
            if (!g->isExpanded())
                g->setExpanded(true, g->property("animations").toBool());
        });
        connect(actionGroup, &mv::gui::GroupAction::collapsed, g, [g] {
            if (g->isExpanded())
                g->setExpanded(false, g->property("animations").toBool());
        });
#endif
        layout->addWidget(g);
        auto *f = new AssistantControls::FormLayout(g->body());
        f->setContentsMargins(4, 4, 4, 8);
        f->setRowWrapPolicy(QFormLayout::WrapLongRows);
        f->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        return f;
    };
    auto row = [&](QFormLayout *f, const QString &label, QWidget *w, const QString &tip) {
        f->addRow(label, AssistantControls::container(w));
        chatTip(w, label, tip);
    };
    auto *appearance = group("Appearance");
    themeChoice_ = AssistantControls::option(
        actionOwner, "Theme",
        {"System", "Light", "Dark", "Midnight", "Nord", "Graphite", "Sepia", "Rose", "High contrast"},
        settingsPanel_);
    themeChoice_->setObjectName("themeChoice");
    row(appearance, "Theme", themeChoice_,
        "Complete palettes for the canvas, text, code, menus and controls.");
    auto *accent = tool("settings", "Accent colour…", "Choose an accessible accent colour", settingsPanel_);
    appearance->addRow(accent);
    AssistantControls::onTrigger(accent, this, [this] {
        auto c = QColorDialog::getColor(theme_.accent, this, "Accent colour");
        if (c.isValid()) {
            accent_ = c;
            saveSettings();
            applyTheme();
        }
    });
    fontSize_ = AssistantControls::integer(actionOwner, "Text size", 10, 20, settingsPanel_);
    fontSize_->setRange(10, 20);
    fontSize_->setSuffix(" pt");
    row(appearance, "Text size", fontSize_,
        "Change message text size without changing the host application.");
    chatWidth_ = AssistantControls::integer(actionOwner, "Reading width", 420, 1400, settingsPanel_);
    chatWidth_->setRange(420, 1400);
    chatWidth_->setSingleStep(40);
    row(appearance, "Reading width", chatWidth_,
        "Maximum message width. Narrow columns improve readability on wide screens.");
    compact_ = AssistantControls::toggle(actionOwner, "Compact spacing", settingsPanel_);
    timestamps_ = AssistantControls::toggle(actionOwner, "Show timestamps", settingsPanel_);
    animations_ = AssistantControls::toggle(actionOwner, "Subtle animations", settingsPanel_);
    autoScroll_ = AssistantControls::toggle(actionOwner, "Follow new replies at the bottom", settingsPanel_);
    showVisuals_ = AssistantControls::toggle(actionOwner, "Show native charts in answers", settingsPanel_);
    for (auto *w : {compact_, timestamps_, animations_, autoScroll_, showVisuals_})
        appearance->addRow(w);
    chatTip(compact_, "Compact spacing", "Reduce padding and gaps while preserving readable message text.");
    chatTip(timestamps_, "Message timestamps", "Show when each message was created in your local time.");
    chatTip(animations_, "Reduced motion",
            "Turn off subtle group transitions and progress motion if you prefer a still interface.");
    chatTip(autoScroll_, "Follow replies",
            "Follow only while you are near the end. Scrolling upward keeps your reading position.");
    chatTip(showVisuals_, "Native charts", "Render supported chart data without a browser or script engine.");
    refreshRate_ = AssistantControls::option(
        actionOwner, "Streaming refresh",
        {"Smooth · 16 ms", "Balanced · 32 ms", "Efficient · 64 ms", "Low power · 100 ms"}, settingsPanel_);
    row(appearance, "Stream refresh", refreshRate_,
        "Batch visual updates independently of generation speed. Efficient and Low power reduce Markdown "
        "parsing and UI redraws.");
#ifdef ASSISTANT_MANIVAULT_ACTIONS
    auto toolbarSettings = assistantSettings();
    messageTools_ = toolbarSettings.value("messageTools", messageTools_).toStringList();
    auto *messageTools = new mv::gui::OptionsAction(actionOwner, "Message toolbar",
                                                    {"Copy", "Branch", "Bookmark"}, messageTools_);
    qobject_cast<mv::gui::GroupAction *>(actionOwner)->addAction(messageTools);
    auto *messageToolsWidget = messageTools->createWidget(settingsPanel_, mv::gui::OptionsAction::ComboBox);
    appearance->addRow("Message tools", messageToolsWidget);
    connect(messageTools, &mv::gui::OptionsAction::selectedOptionsChanged, this,
            [this](const QStringList &options) {
                messageTools_ = options;
                saveSettings();
                for (auto *button : messagesCanvas_->findChildren<QPushButton *>()) {
                    const auto key = button->property("messageTool").toString();
                    if (!key.isEmpty())
                        AssistantControls::setVisible(button, options.contains(key));
                }
            });
#endif
    auto *answers = group("Answer preferences");
    performance_ = AssistantControls::option(actionOwner, "Performance preset",
                                             {"Fast", "Balanced", "Detailed", "Custom"}, settingsPanel_);
    row(answers, "Preset", performance_,
        "Fast uses a shorter answer budget and fewer source excerpts. GPU acceleration is selected at build "
        "time.");
    answerStyle_ = AssistantControls::option(
        actionOwner, "Answer format",
        {"Concise", "Numbered steps", "Detailed", "Comparison table", "Explain simply"}, settingsPanel_);
    row(answers, "Format", answerStyle_,
        "Choose the response structure. The model still needs supporting evidence.");
    maxTokens_ = AssistantControls::integer(actionOwner, "Answer limit", 32, 1024, settingsPanel_);
    maxTokens_->setRange(32, 1024);
    maxTokens_->setSingleStep(32);
    row(answers, "Answer limit", maxTokens_, "Maximum generated tokens. Shorter answers finish sooner.");
    topK_ = AssistantControls::integer(actionOwner, "Source excerpts", 1, 8, settingsPanel_);
    topK_->setRange(1, 8);
    row(answers, "Source excerpts", topK_,
        "More excerpts provide broader evidence but increase context processing.");
    contextTokens_ = AssistantControls::integer(actionOwner, "Context limit", 1024, 8192, settingsPanel_);
    contextTokens_->setRange(1024, 8192);
    contextTokens_->setSingleStep(512);
    row(answers, "Context limit", contextTokens_,
        "Space shared by documents, conversation history and the response.");
    historyTurns_ = AssistantControls::integer(actionOwner, "History turns", 0, 20, settingsPanel_);
    historyTurns_->setRange(0, 20);
    row(answers, "History turns", historyTurns_,
        "Previous question/answer turns included from this conversation.");
    temperature_ = AssistantControls::decimal(actionOwner, "Temperature", 0, 1, settingsPanel_);
    temperature_->setRange(0, 1);
    temperature_->setSingleStep(.1);
    row(answers, "Temperature", temperature_,
        "Lower values are more deterministic; zero uses greedy sampling.");
    repeatPenalty_ = AssistantControls::decimal(actionOwner, "Repeat penalty", 1, 1.5, settingsPanel_);
    repeatPenalty_->setRange(1, 1.5);
    repeatPenalty_->setSingleStep(.05);
    row(answers, "Repeat penalty", repeatPenalty_,
        "Discourage repeated words and looping passages. One disables the penalty.");
    includeSources_ =
        AssistantControls::toggle(actionOwner, "Ground answers in local documents", settingsPanel_);
    chatMl_ = AssistantControls::toggle(actionOwner, "Qwen ChatML prompt format", settingsPanel_);
    prepareWhileTyping_ =
        AssistantControls::toggle(actionOwner, "Prepare context while typing", settingsPanel_);
    chatTip(includeSources_, "Ground answers", "Search the local index and pass matching excerpts to the model.");
    chatTip(chatMl_, "Qwen ChatML", "Use the chat template expected by the bundled Qwen model.");
    chatTip(prepareWhileTyping_, "Prepare context while typing",
            "Start reading source context after a typing pause to reduce the wait after sending.");
    for (auto *w : {includeSources_, chatMl_, prepareWhileTyping_})
        answers->addRow(w);
    customInstructions_ = AssistantControls::text(actionOwner, "Response instructions", settingsPanel_);
    customInstructions_->setObjectName("customInstructions");
    customInstructions_->setAcceptRichText(false);
    customInstructions_->setFixedHeight(85);
    customInstructions_->setPlaceholderText("Optional preferences, e.g. explain for beginners");
    answers->addRow("Instructions", customInstructions_);
    auto *interaction = group("Interaction and privacy");
    enterMode_ = AssistantControls::option(actionOwner, "Send shortcut",
                                           {"Enter sends", "Ctrl / Cmd + Enter sends"}, settingsPanel_);
    enterMode_->setObjectName("enterMode");
    row(interaction, "Send shortcut", enterMode_,
        "Shift+Enter always inserts a new line. Ctrl/Cmd+Enter always sends.");
    saveHistory_ =
        AssistantControls::toggle(actionOwner, "Save conversations on this device", settingsPanel_);
    interaction->addRow(saveHistory_);
    chatTip(saveHistory_, "Local history",
            "Saved chats are compressed in a separate local database. This does not modify the model's "
            "knowledge index.");
    auto *runtime = group("Local runtime");
    packageLabel_ = caption("Not selected");
    runtimeLabel_ = caption("Not selected");
    runtime->addRow("Package", packageLabel_);
    auto *pick = tool("file", "Choose package…", "Select a trainer release folder", settingsPanel_);
    runtime->addRow(pick);
    AssistantControls::onTrigger(pick, this, [this] {
        auto p = QFileDialog::getExistingDirectory(this, "Model package", packagePath_);
        if (!p.isEmpty()) {
            cancelForNavigation();
            packagePath_ = p;
            saveSettings();
            refreshPaths();
        }
    });
    runtime->addRow("Runtime", runtimeLabel_);
    auto *historyFolder =
        tool("file", "Open conversation folder",
             "Open this user's installation-specific ManiVaultChatAssistant application-data folder",
             settingsPanel_);
    runtime->addRow(historyFolder);
    AssistantControls::onTrigger(historyFolder, this, [this] {
        const auto folder = QFileInfo(store_.path()).absolutePath();
        if (QDir().mkpath(folder))
            QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    });
    auto *importHistory = tool("file", "Import previous history…",
                               "Read a selected assistant SQLite history database without changing it. "
                               "Existing conversations in this installation are retained.",
                               settingsPanel_);
    runtime->addRow(importHistory);
    AssistantControls::onTrigger(importHistory, this, [this] {
        const auto source = QFileDialog::getOpenFileName(this, "Import assistant history", {},
                                                         "Assistant history (*.sqlite3 *.sqlite *.db)");
        if (source.isEmpty())
            return;
        QString error;
        if (!store_.importHistory(source, &error))
            setStatus("History import failed: " + error, false);
        else {
            refreshChats();
            setStatus("History imported · Original file unchanged", true);
        }
    });
    auto *diagnostics =
        tool("chart", "Copy diagnostics",
             "Copy device, package, history location and response timing. No message text is included.",
             settingsPanel_);
    runtime->addRow(diagnostics);
    AssistantControls::onTrigger(diagnostics, this, [this] { copyDiagnostics(); });
    auto *choose = tool("settings", "Choose runtime…", "Select assistant-runtime, or a compatible legacy CLI",
                        settingsPanel_);
    runtime->addRow(choose);
    AssistantControls::onTrigger(choose, this, [this] {
        auto p = QFileDialog::getOpenFileName(this, "Local inference executable", runtimePath_);
        if (!p.isEmpty()) {
            cancelForNavigation();
            runtimePath_ = p;
            saveSettings();
            refreshPaths();
        }
    });
    auto *reset = tool("retry", "Reset appearance",
                       "Restore appearance settings while retaining chats and model paths", settingsPanel_);
    layout->addWidget(reset);
    AssistantControls::onTrigger(reset, this, [this] {
        {
            QScopedValueRollback<bool> batch(loadingSettings_, true);
            AssistantControls::select(themeChoice_, "System");
            fontSize_->setValue(11);
            chatWidth_->setValue(880);
            accent_ = QColor();
        }
        saveSettings();
        applyTheme();
    });
    layout->addStretch();
    settingsScroll_->setWidget(contents);
    frame->addWidget(settingsScroll_);
    connect(settingsSearch_, &QLineEdit::textChanged, this, [contents](const QString &term) {
        for (auto *group : contents->findChildren<ChatGroup *>(QString(), Qt::FindDirectChildrenOnly)) {
            QString searchable = group->header()->text();
            for (auto *label : group->findChildren<QLabel *>())
                searchable += ' ' + label->text();
            for (auto *control : group->findChildren<QWidget *>())
                searchable += ' ' + control->toolTip() + ' ' + control->accessibleName();
            if (!group->property("searching").toBool() && !term.isEmpty())
                group->setProperty("beforeSearchExpanded", group->isExpanded());
            group->setProperty("searching", !term.isEmpty());
            group->setVisible(term.isEmpty() || searchable.contains(term.trimmed(), Qt::CaseInsensitive));
            if (!term.isEmpty())
                group->setExpanded(true);
            else
                group->setExpanded(group->property("beforeSearchExpanded").toBool());
        }
    });
    auto changed = [this] {
        if (loadingSettings_)
            return;
        saveSettings();
        applyTheme();
    };
    for (auto *w : {themeChoice_, answerStyle_, enterMode_})
        AssistantControls::onChanged(w, this, changed);
    AssistantControls::onChanged(refreshRate_, this, [this] {
        if (renderTimer_)
            renderTimer_->setInterval(streamRefreshMs(refreshRate_));
        saveSettings();
    });
    for (auto *w : {fontSize_, chatWidth_, maxTokens_, topK_, contextTokens_, historyTurns_})
        AssistantControls::onChanged(w, this, changed);
    for (auto *w : {temperature_, repeatPenalty_})
        AssistantControls::onChanged(w, this, changed);
    for (auto *w : {compact_, timestamps_, animations_, autoScroll_, showVisuals_, includeSources_, chatMl_,
                    prepareWhileTyping_, saveHistory_})
        AssistantControls::onChanged(w, this, changed);
    AssistantControls::onChanged(customInstructions_, this, [this] {
        if (!loadingSettings_)
            saveSettings();
    });
    AssistantControls::onOption(performance_, this, [this](int p) {
        if (loadingSettings_)
            return;
        if (p == 3) {
            saveSettings();
            return;
        }
        {
            QScopedValueRollback<bool> batch(loadingSettings_, true);
            maxTokens_->setValue(p == 0 ? 128 : p == 1 ? 256 : 512);
            topK_->setValue(p == 0 ? 2 : p == 1 ? 3 : 4);
            contextTokens_->setValue(p == 2 ? 8192 : 4096);
            temperature_->setValue(p == 0 ? 0 : .2);
            AssistantControls::select(answerStyle_, p == 2 ? "Detailed" : "Concise");
        }
        saveSettings();
        applyTheme();
    });
}

void ChatPanel::loadSettings() {
    auto s = assistantSettings();
    packagePath_ = s.value("packagePath").toString();
    runtimePath_ = s.value("runtimePath").toString();
    suggestionsPath_ = s.value("suggestionsFile").toString();
    accent_ = QColor(s.value("accent").toString());
    AssistantControls::select(themeChoice_, s.value("theme", "System").toString());
    fontSize_->setValue(s.value("fontSize", 11).toInt());
    chatWidth_->setValue(s.value("chatWidth", 880).toInt());
    compact_->setChecked(s.value("compact", false).toBool());
    timestamps_->setChecked(s.value("timestamps", false).toBool());
    animations_->setChecked(s.value("animations", true).toBool());
    AssistantControls::select(refreshRate_, std::clamp(s.value("refreshRate", 1).toInt(), 0, 3));
    autoScroll_->setChecked(s.value("autoScroll", true).toBool());
    showVisuals_->setChecked(s.value("showVisuals", true).toBool());
    AssistantControls::select(performance_, s.value("performance", "Fast").toString());
    AssistantControls::select(answerStyle_, s.value("answerStyle", "Concise").toString());
    maxTokens_->setValue(s.value("maxTokens", 128).toInt());
    topK_->setValue(s.value("topK", 2).toInt());
    contextTokens_->setValue(s.value("contextTokens", 4096).toInt());
    historyTurns_->setValue(s.value("historyTurns", 4).toInt());
    temperature_->setValue(s.value("temperature", .1).toDouble());
    repeatPenalty_->setValue(s.value("repeatPenalty", 1.1).toDouble());
    includeSources_->setChecked(s.value("includeSources", true).toBool());
    chatMl_->setChecked(s.value("chatMl", true).toBool());
    prepareWhileTyping_->setChecked(s.value("prepareTyping", true).toBool());
    AssistantControls::select(enterMode_, s.value("enterMode", 0).toInt());
    saveHistory_->setChecked(s.value("saveHistory", true).toBool());
    customInstructions_->setPlainText(s.value("instructions").toString());
    sidebarButton_->setChecked(s.value("sidebar", true).toBool());
    pendingGroupFilter_ = s.value("groupFilter", "All groups").toString();
    AssistantControls::select(chatOrder_, std::clamp(s.value("chatOrder", 0).toInt(), 0, 2));
}
void ChatPanel::saveSettings() {
    if (loadingSettings_ || !themeChoice_)
        return;
    auto s = assistantSettings();
    const QVariantMap values{{"packagePath", packagePath_},
                             {"groupFilter", groupFilter_->currentText()},
                             {"chatOrder", chatOrder_->currentIndex()},
                             {"messageTools", messageTools_},
                             {"runtimePath", runtimePath_},
                             {"suggestionsFile", suggestionsPath_},
                             {"accent", accent_.isValid() ? accent_.name() : QString()},
                             {"theme", themeChoice_->currentText()},
                             {"fontSize", fontSize_->value()},
                             {"chatWidth", chatWidth_->value()},
                             {"compact", compact_->isChecked()},
                             {"timestamps", timestamps_->isChecked()},
                             {"animations", animations_->isChecked()},
                             {"refreshRate", refreshRate_->currentIndex()},
                             {"autoScroll", autoScroll_->isChecked()},
                             {"showVisuals", showVisuals_->isChecked()},
                             {"performance", performance_->currentText()},
                             {"answerStyle", answerStyle_->currentText()},
                             {"maxTokens", maxTokens_->value()},
                             {"topK", topK_->value()},
                             {"contextTokens", contextTokens_->value()},
                             {"historyTurns", historyTurns_->value()},
                             {"temperature", temperature_->value()},
                             {"repeatPenalty", repeatPenalty_->value()},
                             {"includeSources", includeSources_->isChecked()},
                             {"chatMl", chatMl_->isChecked()},
                             {"prepareTyping", prepareWhileTyping_->isChecked()},
                             {"enterMode", enterMode_->currentIndex()},
                             {"saveHistory", saveHistory_->isChecked()},
                             {"instructions", customInstructions_->toPlainText().left(2000)},
                             {"sidebar", sidebarButton_->isChecked()}};
    for (auto i = values.begin(); i != values.end(); ++i)
        s.setValue(i.key(), i.value());
}

void ChatPanel::applyTheme() {
    if (!themeChoice_)
        return;
    theme_ = ChatTheme::resolve(themeChoice_->currentText(), accent_,
                                qApp->palette().color(QPalette::Window).lightnessF() < .5);
    QPalette p = palette();
    for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        p.setColor(group, QPalette::Window, theme_.background);
        p.setColor(group, QPalette::Base, theme_.surface);
        p.setColor(group, QPalette::AlternateBase, theme_.elevated);
        p.setColor(group, QPalette::WindowText, theme_.text);
        p.setColor(group, QPalette::Text, theme_.text);
        p.setColor(group, QPalette::Button, theme_.surface);
        p.setColor(group, QPalette::ButtonText, theme_.text);
        p.setColor(group, QPalette::PlaceholderText, theme_.muted);
        p.setColor(group, QPalette::Highlight, theme_.accent);
        p.setColor(group, QPalette::HighlightedText, theme_.accentText);
        p.setColor(group, QPalette::ToolTipBase, theme_.surface);
        p.setColor(group, QPalette::ToolTipText, theme_.text);
    }
    setPalette(p);
    setAutoFillBackground(true);
    sideTabs_->setTabIcon(0, chatIcon("chat", theme_.text));
    sideTabs_->setTabIcon(1, chatIcon("file", theme_.text));
    messagesCanvas_->setPalette(p);
    messagesCanvas_->setAutoFillBackground(true);
    scroll_->viewport()->setPalette(p);
    scroll_->viewport()->setAutoFillBackground(true);
    const QString css = QString(R"(
      QWidget#assistantRoot {background:%1;color:%4;font-size:%11pt;}
      QDialog#assistantPreferences {background:%1;color:%4;}
      QWidget#assistantRoot QWidget, QDialog#assistantPreferences QWidget {color:%4;selection-background-color:%8;selection-color:%9;}
      QFrame#toolbar,QFrame#sidebar,QFrame#settingsPane,QFrame#composer {background:%2;border:1px solid %6;border-radius:12px;}
      QWidget#messageCanvas,QWidget#chatColumn,QScrollArea,QScrollArea>QWidget>QWidget {background:%1;border:none;}
      QWidget#suggestionsCanvas {background:%2;}
      QFrame#assistantMessage {background:%2;border:1px solid %6;border-radius:12px;}
      QFrame#userMessage {background:%7;border:1px solid %6;border-radius:12px;}
      QLabel {color:%4;background:transparent;}
      QLabel#heading {font-size:14pt;font-weight:700;}
      QLabel#sectionTitle {font-size:12pt;font-weight:700;}
      QLabel#muted,QLabel#runtimeStatus,QLabel#contextChip {color:%5;font-size:9pt;}
      QLabel#roleLabel {color:%5;font-size:9pt;font-weight:600;}
      QTextBrowser {background:transparent;color:%4;border:none;padding:0;}
      QTextEdit#chatInput {background:transparent;color:%4;border:none;padding:2px;}
      QPushButton,QToolButton {background:%2;color:%4;border:1px solid %6;border-radius:7px;padding:%10px 9px;}
      QPushButton:hover,QToolButton:hover {background:%3;border-color:%8;}
      QPushButton:checked,QToolButton:checked {background:%3;border-color:%8;}
      QPushButton:disabled,QToolButton:disabled {color:%5;border-color:%6;}
      QPushButton#sendButton {background:%8;color:%9;font-weight:600;border-color:%8;padding:8px 15px;}
      QPushButton#sendButton:hover {border:1px solid %4;}
      QPushButton[smallAction="true"] {border:none;background:transparent;padding:2px;min-width:22px;max-width:28px;}
      QLineEdit,QSpinBox,QDoubleSpinBox,QComboBox,QTextEdit#customInstructions {background:%2;color:%4;border:1px solid %6;border-radius:6px;padding:6px;}
      QComboBox QAbstractItemView {background:%2;color:%4;selection-background-color:%3;selection-color:%4;border:1px solid %6;}
      QComboBox::drop-down {border:none;width:20px;}
      QCheckBox {color:%4;spacing:7px;padding:3px 0;}
      QCheckBox::indicator {width:15px;height:15px;background:%2;border:1px solid %5;border-radius:3px;}
      QCheckBox::indicator:checked {background:%8;border-color:%8;}
      QListWidget#chatList {background:transparent;border:none;outline:none;color:%4;}
      QListWidget#chatList::item {padding:10px 8px;border-radius:7px;border:1px solid transparent;}
      QListWidget#chatList::item:selected {background:%3;border-color:%6;color:%4;}
      QListWidget#chatList::item:hover {background:%3;}
      QTabWidget::pane {border:none;background:transparent;}
      QTabBar::tab {background:transparent;color:%5;padding:8px 10px;border-bottom:2px solid transparent;}
      QTabBar::tab:selected {color:%4;border-bottom-color:%8;}
      QFrame#collapsibleGroup {background:transparent;border:none;}
      QFrame#collapsibleGroup>QToolButton {text-align:left;border:none;background:transparent;font-weight:600;padding:6px 2px;}
      QMenu {background:%2;color:%4;border:1px solid %6;padding:5px;}
      QMenu::item {padding:7px 22px 7px 10px;border-radius:4px;}
      QMenu::item:selected {background:%3;color:%4;}
      QScrollBar:vertical {background:transparent;width:8px;margin:0;}
      QScrollBar::handle:vertical {background:%6;min-height:32px;border-radius:4px;}
      QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical {height:0;}
      QSplitter::handle {background:transparent;width:6px;}
      QProgressBar#taskProgress {background:%6;border:none;border-radius:1px;}
      QProgressBar#taskProgress::chunk {background:%8;border-radius:1px;}
      QToolTip {background:%2;color:%4;border:1px solid %6;padding:8px;}
    )")
                            .arg(theme_.background.name(), theme_.surface.name(), theme_.elevated.name(),
                                 theme_.text.name(), theme_.muted.name(), theme_.border.name(),
                                 theme_.user.name(), theme_.accent.name(), theme_.accentText.name())
                            .arg(compact_->isChecked() ? "4" : "6")
                            .arg(QString::number(fontSize_->value()));
    setStyleSheet(css);
    // SDK action labels maintain their own style sheet from the host palette.
    // Override those labels locally so a dark assistant remains readable inside
    // a light host, including after the label's hover/enabled refresh.
    for (auto *label : settingsPanel_->findChildren<QLabel *>("Label")) {
        label->setProperty("assistantActionLabel", true);
        label->installEventFilter(this);
        label->setStyleSheet(QString("QLabel { color:%1; background:transparent; }").arg(theme_.text.name()));
    }
    if (settingsDialog_) {
        settingsDialog_->setPalette(p);
        settingsDialog_->setStyleSheet(css);
    }
    for (auto *b : findChildren<QPushButton *>()) {
        auto name = b->property("iconName").toString();
        if (!name.isEmpty()) {
            b->setIcon(chatIcon(name, b == sendButton_ ? theme_.accentText : theme_.text));
            if (auto *action = AssistantControls::action(b))
                action->setIcon(b->icon());
        }
        fitQuestionButton(b);
    }
    if (auto *brand = findChild<QLabel *>("brandIcon"))
        brand->setPixmap(chatIcon("chat", theme_.accent).pixmap(24, 24));
    for (int i = 0; i < chats_->count(); ++i) {
        auto *item = chats_->item(i);
        item->setIcon(chatIcon(item->data(Qt::UserRole + 1).toBool() ? "star" : "chat", theme_.muted));
    }
    for (auto *g : findChildren<ChatGroup *>())
        g->setProperty("animations", animations_->isChecked());
    composerHint_->setText(editIndex_ >= 0 ? "Editing an earlier question · Submit creates a branch"
                           : enterMode_->currentIndex() == 0
                               ? "Enter to send · Shift+Enter for a new line"
                               : "Ctrl/Cmd+Enter to send · Enter for a new line");
    messagesLayout_->setSpacing(compact_->isChecked() ? 7 : 12);
    refreshMessagesAppearance();
}

void ChatPanel::refreshMessagesAppearance() {
    for (auto *row : messagesCanvas_->findChildren<QWidget *>("messageRow", Qt::FindDirectChildrenOnly)) {
        row->setMaximumWidth(chatWidth_->value());
        for (auto *browser : row->findChildren<ChatTextBrowser *>())
            browser->setChatMarkdown(browser->property("assistantMarkdown").toString(), theme_,
                                     fontSize_->value());
        for (auto *timestamp : row->findChildren<QLabel *>("timestamp"))
            timestamp->setVisible(timestamps_->isChecked());
    }
}
void ChatPanel::toggleSettings(bool show) {
    if (!show) {
        AssistantControls::setVisible(settingsPanel_, false);
        if (settingsDialog_)
            AssistantControls::setVisible(settingsDialog_, false);
        if (settingsPanel_->parentWidget() != splitter_) {
            settingsPanel_->setParent(splitter_);
            splitter_->addWidget(settingsPanel_);
            AssistantControls::setVisible(settingsPanel_, false);
        }
        return;
    }
    if (width() < 900) {
        if (!settingsDialog_) {
            settingsDialog_ = new QDialog(this, Qt::Tool);
            settingsDialog_->setObjectName("assistantPreferences");
            settingsDialog_->setWindowTitle("Assistant preferences");
            settingsDialog_->setModal(false);
            settingsDialog_->resize(380, 650);
            auto *l = new AssistantControls::VBoxLayout(settingsDialog_);
            l->setContentsMargins(0, 0, 0, 0);
            connect(settingsDialog_, &QDialog::finished, this,
                    [this] { settingsButton_->setChecked(false); });
        }
        settingsPanel_->setParent(settingsDialog_);
        settingsDialog_->layout()->addWidget(settingsPanel_);
        settingsPanel_->setMaximumWidth(QWIDGETSIZE_MAX);
        AssistantControls::setVisible(settingsPanel_, true);
        AssistantControls::setVisible(settingsDialog_, true);
        settingsDialog_->raise();
    } else {
        if (settingsDialog_)
            AssistantControls::setVisible(settingsDialog_, false);
        settingsPanel_->setParent(splitter_);
        splitter_->addWidget(settingsPanel_);
        settingsPanel_->setMaximumWidth(380);
        AssistantControls::setVisible(settingsPanel_, true);
        splitter_->setSizes({sidebar_->isVisible() ? 220 : 0, std::max(300, width() - 580), 340});
    }
    applyTheme();
}
void ChatPanel::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    if (sidebar_ && width() < 680 && sidebar_->isVisible()) {
        sidebarButton_->setChecked(false);
        AssistantControls::setVisible(sidebar_, false);
    }
    if (settingsButton_ && settingsButton_->isChecked()) {
        const bool floating = settingsPanel_->parentWidget() == settingsDialog_;
        if ((width() < 900) != floating)
            QTimer::singleShot(0, this, [this] { toggleSettings(true); });
    }
}
void ChatPanel::refreshPaths() {
    retrievedQuestion_.clear();
    retrievedLimit_ = 0;
    retrievedSources_ = {};
    if (!QFileInfo(QDir(packagePath_).filePath("knowledge.db")).isFile() ||
        RuntimePaths::modelFile(packagePath_).isEmpty())
        packagePath_ = RuntimePaths::discoverPackage();
    if (!QFileInfo(runtimePath_).isExecutable() ||
        QFileInfo(runtimePath_).completeBaseName() == "llama-completion")
        runtimePath_ = RuntimePaths::discoverExecutable();
    packageLabel_->setText(packagePath_.isEmpty() ? "Not selected" : QFileInfo(packagePath_).fileName());
    packageLabel_->setToolTip(packagePath_);
    runtimeLabel_->setText(runtimePath_.isEmpty() ? "Not selected" : QFileInfo(runtimePath_).fileName());
    runtimeLabel_->setToolTip(runtimePath_);
    QString error;
    const bool ready = index_.open(QDir(packagePath_).filePath("knowledge.db"), &error) &&
                       !RuntimePaths::modelFile(packagePath_).isEmpty() &&
                       QFileInfo(runtimePath_).isExecutable();
    inference_.setPaths(packagePath_, runtimePath_);
    AssistantControls::setEnabled(sendButton_, ready && !inference_.isBusy());
    AssistantControls::setEnabled(pauseButton_, inference_.supportsPause());
    setStatus(ready ? (inference_.isReady() ? "Ready · " + inference_.runtimeDescription()
                                            : "Preparing local assistant…")
                    : "Setup needed · Choose a model package and runtime",
              ready);
    if (!error.isEmpty())
        statusLabel_->setToolTip(error);
}
RetrievalResult ChatPanel::retrieveSources(const QString &question, const InferenceSettings &settings) {
    if (!settings.useKnowledge || !index_.isOpen())
        return {};
    if (question == retrievedQuestion_ && settings.topK == retrievedLimit_)
        return retrievedSources_;
    const auto result = index_.retrieve(question, settings.topK);
    if (result.error.isEmpty()) {
        retrievedQuestion_ = question;
        retrievedLimit_ = settings.topK;
        retrievedSources_ = result;
    }
    return result;
}
void ChatPanel::setStatus(const QString &text, bool ready) {
    statusLabel_->setText(text);
    statusLabel_->setToolTip(text);
    statusLabel_->setProperty("ready", ready);
}
void ChatPanel::updateProgress(const QString &stage, int current, int total) {
    if (!waiting_ && stage != "loading" && stage != "search")
        return;
    QString label = stage == "loading"    ? "Preparing model"
                    : stage == "search"   ? "Searching local documents"
                    : stage == "context"  ? "Reading context"
                    : stage == "paused"   ? "Paused"
                    : stage == "stopping" ? "Stopping"
                                          : "Writing answer";
    if (stage == "generating" && current > 0)
        label += QString(" · %1 tokens").arg(current);
    progressLabel_->setText(label);
    chatTip(progress_, label,
            total > 0 ? QString("%1 of %2 completed. Progress describes this stage, not a time estimate.")
                            .arg(current)
                            .arg(total)
                      : "The duration of this stage is unknown. Pause or stop from the composer.");
#ifdef ASSISTANT_MANIVAULT_ACTIONS
    if (hostTask_) {
        hostTask_->setDescription(label);
        if (total > 0) {
            hostTask_->setRunning();
            hostTask_->setProgress(std::clamp(float(current) / total, 0.0f, 1.0f), label);
        } else
            hostTask_->setRunningIndeterminate();
    }
#endif
    if (total > 0) {
        progress_->setRange(0, total);
        progress_->setValue(current);
    } else if (stage == "paused" || !animations_->isChecked()) {
        progress_->setRange(0, 100);
        progress_->setValue(0);
    } else
        progress_->setRange(0, 0);
}

void ChatPanel::copyDiagnostics() {
    const QString text =
        QString("Knowledge Assistant 1.0.0\nQt: %1\nDevice: %2\nModel package: %3\nRuntime: %4\nHistory: "
                "%5\nPreset: %6\nContext/answer limits: %7 / %8\nUI refresh: %9 ms\nLast response: %10\n")
            .arg(QString::fromLatin1(qVersion()), inference_.runtimeDescription(), packagePath_, runtimePath_,
                 store_.path(), performance_->currentText())
            .arg(contextTokens_->value())
            .arg(maxTokens_->value())
            .arg(renderTimer_ ? renderTimer_->interval() : 32)
            .arg(statisticsLabel_->text());
    QApplication::clipboard()->setText(text);
    setStatus("Diagnostics copied · No messages included", true);
}

void ChatPanel::showCommands() {
    if (auto *existing = findChild<QDialog *>("assistantCommands")) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }
    auto *dialog = new QDialog(this);
    dialog->setObjectName("assistantCommands");
    dialog->setWindowTitle("Assistant commands");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setModal(false);
    dialog->resize(520, 400);
    dialog->setPalette(palette());
    dialog->setStyleSheet(styleSheet());
    auto *layout = new AssistantControls::VBoxLayout(dialog);
    auto *filter = AssistantControls::line(dialog, "Find a command", dialog);
    filter->setObjectName("commandSearch");
    filter->setPlaceholderText("Type a command…");
    filter->setClearButtonEnabled(true);
    layout->addWidget(filter);
    auto *list = new QListWidget(dialog);
    list->setObjectName("commandList");
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(list, 1);
    layout->addWidget(caption("Enter to run · Escape to close", dialog));
    const auto commands = commands_;
    auto refresh = [list, filter, commands] {
        list->clear();
        for (int i = 0; i < commands.size(); ++i) {
            auto *action = commands[i].data();
            if (!action || !action->isEnabled() ||
                !action->text().contains(filter->text(), Qt::CaseInsensitive))
                continue;
            auto *item = new QListWidgetItem(action->icon(), action->text(), list);
            item->setData(Qt::UserRole, i);
            item->setToolTip(action->toolTip());
        }
        if (list->count())
            list->setCurrentRow(0);
    };
    auto run = [dialog, list, commands] {
        if (auto *item = list->currentItem()) {
            QPointer<QAction> action = commands.value(item->data(Qt::UserRole).toInt());
            dialog->close();
            if (action && action->isEnabled())
                action->trigger();
        }
    };
    connect(filter, &QLineEdit::textChanged, dialog, refresh);
    connect(filter, &QLineEdit::returnPressed, dialog, run);
    connect(list, &QListWidget::itemActivated, dialog, [run] { run(); });
    for (auto key : {Qt::Key_Down, Qt::Key_Up}) {
        auto *shortcut = new QShortcut(QKeySequence(key), dialog);
        connect(shortcut, &QShortcut::activated, dialog, [list, key] {
            if (list->count())
                list->setCurrentRow(
                    std::clamp(list->currentRow() + (key == Qt::Key_Down ? 1 : -1), 0, list->count() - 1));
        });
    }
    refresh();
    dialog->show();
    filter->setFocus();
    if (animations_->isChecked()) {
        auto *fade = new QPropertyAnimation(dialog, "windowOpacity", dialog);
        fade->setDuration(120);
        fade->setStartValue(.65);
        fade->setEndValue(1.0);
        fade->start(QAbstractAnimation::DeleteWhenStopped);
    }
}

void ChatPanel::editMessage(int index) {
    if (waiting_ || index < 0 || index >= chat_.messages.size() || chat_.messages[index].role != "user")
        return;
    if (editIndex_ < 0)
        drafts_[chat_.id] = input_->toPlainText();
    editIndex_ = index;
    input_->setPlainText(chat_.messages[index].text);
    input_->setFocus();
    input_->moveCursor(QTextCursor::End);
    AssistantControls::setVisible(cancelEditButton_, true);
    sendButton_->setText("Submit edit");
    if (auto *action = AssistantControls::action(sendButton_))
        action->setText("Submit edit");
    applyTheme();
}
void ChatPanel::editSavedMessage(int index) {
    if (waiting_ || index < 0 || index >= chat_.messages.size())
        return;
    const auto conversationId = chat_.id;
    const auto messageId = chat_.messages[index].id;
    auto *dialog = new QDialog(this);
    dialog->setObjectName("savedMessageEditor");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("Edit saved message");
    dialog->setModal(false);
    dialog->resize(600, 430);
    dialog->setPalette(palette());
    dialog->setStyleSheet(styleSheet());
    auto *layout = new AssistantControls::VBoxLayout(dialog);
    layout->addWidget(caption("Edit the saved text. Existing replies are retained. Use Edit and resubmit on "
                              "a question to generate a new branch.",
                              dialog));
    auto *editor = AssistantControls::text(dialog, "Message text", dialog);
    editor->setObjectName("savedMessageText");
    editor->setAcceptRichText(false);
    editor->setPlainText(chat_.messages[index].text);
    layout->addWidget(editor, 1);
    auto *buttons = new AssistantControls::HBoxLayout;
    buttons->addStretch();
    auto *cancel = tool("close", "Cancel", "Close without changing the saved message", dialog);
    AssistantControls::onTrigger(cancel, dialog, [dialog] { dialog->reject(); });
    buttons->addWidget(cancel);
    auto *save = tool("file", "Save changes",
                      "Save text, mark it as edited and preserve the rest of the conversation", dialog);
    save->setObjectName("saveMessageEdit");
    buttons->addWidget(save);
    layout->addLayout(buttons);
    AssistantControls::onTrigger(save, dialog, [this, dialog, editor, conversationId, messageId] {
        if (waiting_ && chat_.id == conversationId) {
            setStatus("Stop the response before saving this edit", false);
            return;
        }
        auto target = chat_.id == conversationId              ? chat_
                      : memoryChats_.contains(conversationId) ? memoryChats_.value(conversationId)
                                                              : store_.load(conversationId);
        auto found = std::find_if(target.messages.begin(), target.messages.end(),
                                  [messageId](const auto &message) { return message.id == messageId; });
        if (target.id.isEmpty() || found == target.messages.end()) {
            setStatus("The message is no longer available", false);
            return;
        }
        if (editor->toPlainText().trimmed().isEmpty()) {
            setStatus("A saved message cannot be empty", false);
            return;
        }
        if (editor->toPlainText() == found->text) {
            dialog->accept();
            return;
        }
        found->text = editor->toPlainText();
        found->edited = true;
        if (found->role == "assistant")
            found->sources.clear();
        saveConversation(target);
        if (chat_.id == target.id) {
            followTail_ = false;
            renderMessages();
        }
        dialog->accept();
    });
    auto *saveShortcut = new QShortcut(QKeySequence::Save, dialog);
    connect(saveShortcut, &QShortcut::activated, save, &QPushButton::click);
    chatTip(save, "Save message changes", "Save this message's text without regenerating the conversation.",
            "Ctrl/Cmd+S");
    dialog->show();
    editor->setFocus();
}
void ChatPanel::cancelEdit() {
    if (editIndex_ < 0)
        return;
    editIndex_ = -1;
    input_->setPlainText(drafts_.take(chat_.id));
    AssistantControls::setVisible(cancelEditButton_, false);
    sendButton_->setText("Send");
    if (auto *action = AssistantControls::action(sendButton_))
        action->setText("Send");
    applyTheme();
}

QWidget *ChatPanel::messageWidget(const ChatMessage &message, int index) {
    auto *outer = new QWidget(messagesCanvas_);
    outer->setObjectName("messageRow");
    outer->setProperty("messageText", message.text);
    outer->setMaximumWidth(chatWidth_->value());
    outer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    auto *line = new AssistantControls::HBoxLayout(outer);
    line->setContentsMargins(0, 0, 0, 0);
    const bool user = message.role == "user";
    if (user)
        line->addStretch(1);
    auto *bubble = new QFrame(outer);
    bubble->setObjectName(user ? "userMessage" : "assistantMessage");
    if (user)
        bubble->setMaximumWidth(std::max(320, chatWidth_->value() - 100));
    auto *content = new AssistantControls::VBoxLayout(bubble);
    content->setContentsMargins(compact_->isChecked() ? 12 : 16, 10, compact_->isChecked() ? 12 : 16, 10);
    content->setSpacing(7);
    auto *toolbar = new AssistantControls::HBoxLayout;
    auto *role =
        new QLabel((user ? QString("YOU") : QString("ASSISTANT")) + (message.edited ? " · Edited" : ""));
    role->setObjectName("roleLabel");
    toolbar->addWidget(role);
    auto *time =
        caption(message.created.isValid() ? message.created.toLocalTime().toString("hh:mm") : QString());
    time->setObjectName("timestamp");
    time->setVisible(timestamps_->isChecked());
    toolbar->addWidget(time);
    toolbar->addStretch();
    auto small = [&](const QString &icon, const QString &tip, auto callback) {
        auto *b = tool(icon, "", tip, bubble);
        b->setProperty("smallAction", true);
        const QString key = icon == "copy" ? "Copy" : icon == "branch" ? "Branch" : "Bookmark";
        b->setProperty("messageTool", key);
        AssistantControls::setVisible(b, messageTools_.contains(key));
        AssistantControls::onTrigger(b, this, callback);
        toolbar->addWidget(b);
        return b;
    };
    small("copy", "Copy message as Markdown",
          [text = message.text] { QApplication::clipboard()->setText(text); });
    small("branch", "Branch a new conversation from this message", [this, index] { branchChat(index); });
    auto *star = small("star", "Bookmark this message", [this, index] {
        if (index >= 0 && index < chat_.messages.size()) {
            chat_.messages[index].starred = !chat_.messages[index].starred;
            saveChat();
            renderMessages();
        }
    });
    star->setProperty("bookmarked", message.starred);
    auto *more = tool("down", "", "Message actions", bubble);
    more->setProperty("smallAction", true);
    auto *menu = new QMenu(more);
    menu->setObjectName("messageContextMenu");
    auto *copyMessage = AssistantControls::command(menu, "Copy message", chatIcon("copy", theme_.text));
    connect(copyMessage, &QAction::triggered, this,
            [text = message.text] { QApplication::clipboard()->setText(text); });
    auto *branchMessage =
        AssistantControls::command(menu, "Branch from this message", chatIcon("branch", theme_.text));
    connect(branchMessage, &QAction::triggered, this, [this, index] { branchChat(index); });
    auto *bookmarkMessage = AssistantControls::command(
        menu, message.starred ? "Remove bookmark" : "Bookmark message", chatIcon("star", theme_.text));
    connect(bookmarkMessage, &QAction::triggered, this, [this, index] {
        if (index >= 0 && index < chat_.messages.size()) {
            chat_.messages[index].starred = !chat_.messages[index].starred;
            saveChat();
            renderMessages();
        }
    });
    menu->addSeparator();
    auto *editSaved = AssistantControls::command(menu, "Edit saved message…", chatIcon("file", theme_.text));
    connect(editSaved, &QAction::triggered, this, [this, index] { editSavedMessage(index); });
    connect(menu, &QMenu::aboutToShow, this, [this, editSaved] { editSaved->setEnabled(!waiting_); });
    if (!user) {
        auto *retry = AssistantControls::command(menu, "Regenerate from the preceding question");
        connect(retry, &QAction::triggered, this, [this, index] { regenerate(index); });
    }
    if (user) {
        auto *resubmit =
            AssistantControls::command(menu, "Edit and resubmit…", chatIcon("file", theme_.text));
        connect(resubmit, &QAction::triggered, this, [this, index] { editMessage(index); });
        auto *edit = AssistantControls::command(menu, "Edit and branch…");
        connect(edit, &QAction::triggered, this, [this, index, text = message.text] {
            branchChat(index - 1);
            input_->setPlainText(text);
            input_->setFocus();
        });
    }
    auto *remove = AssistantControls::command(menu, "Delete this message");
    const auto codeMatch = QRegularExpression("```[^\\n]*\\n([\\s\\S]*?)```").match(message.text);
    if (codeMatch.hasMatch()) {
        auto *copyCode = AssistantControls::command(menu, "Copy code block", chatIcon("copy", theme_.text));
        connect(copyCode, &QAction::triggered, this,
                [code = codeMatch.captured(1)] { QApplication::clipboard()->setText(code); });
    }
    connect(remove, &QAction::triggered, this, [this, index] { deleteMessage(index, false); });
    auto *trim = AssistantControls::command(menu, "Delete from here…");
    connect(trim, &QAction::triggered, this, [this, index] { deleteMessage(index, true); });
    more->setMenu(menu);
    toolbar->addWidget(more);
    content->addLayout(toolbar);
    auto *browser = new ChatTextBrowser(bubble);
    browser->viewport()->setProperty("chatMessageViewport", true);
    browser->viewport()->installEventFilter(this);
    browser->setObjectName("messageText");
    auto contextMenu = [menu](QWidget *widget) {
        widget->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(widget, &QWidget::customContextMenuRequested, menu,
                         [menu, widget](const QPoint &point) { menu->popup(widget->mapToGlobal(point)); });
    };
    contextMenu(bubble);
    contextMenu(outer);
    contextMenu(browser);
    auto *copySelection =
        AssistantControls::command(menu, "Copy selected text", chatIcon("copy", theme_.text));
    connect(copySelection, &QAction::triggered, browser, &QTextBrowser::copy);
    connect(menu, &QMenu::aboutToShow, browser,
            [copySelection, browser] { copySelection->setEnabled(browser->textCursor().hasSelection()); });
    QString markdown = user ? message.text : readableTutorialMarkdown(message.text);
    const auto chart = showVisuals_->isChecked() ? ChatChart::fromMarkdown(markdown) : QJsonObject();
    if (!chart.isEmpty())
        markdown.remove(
            QRegularExpression("```chart\\s*\\n[\\s\\S]*?```", QRegularExpression::CaseInsensitiveOption));
    browser->setProperty("assistantMarkdown", markdown);
    browser->setChatMarkdown(markdown, theme_, fontSize_->value());
    content->addWidget(browser);
    connect(browser, &QTextBrowser::anchorClicked, this, [](const QUrl &url) {
        if (url.scheme() == "https" || url.scheme() == "http")
            QDesktopServices::openUrl(url);
    });
    if (!chart.isEmpty()) {
        auto *visual = new ChatChart(chart, theme_, bubble);
        contextMenu(visual);
        content->addWidget(visual);
        auto *actions = new AssistantControls::HBoxLayout;
        auto *csv = tool("copy", "Copy data", "Copy the chart's numeric data as CSV", bubble);
        AssistantControls::onTrigger(csv, visual,
                                     [visual] { QApplication::clipboard()->setText(visual->csv()); });
        actions->addWidget(csv);
        auto *png = tool("download", "Save chart", "Export this native chart as an image", bubble);
        AssistantControls::onTrigger(png, this, [this, visual] {
            const auto path = QFileDialog::getSaveFileName(this, "Export chart", "chart.png", "PNG (*.png)");
            if (!path.isEmpty())
                visual->grab().save(path);
        });
        actions->addWidget(png);
        menu->addSeparator();
        auto *copyChart =
            AssistantControls::command(menu, "Copy chart data (CSV)", chatIcon("copy", theme_.text));
        connect(copyChart, &QAction::triggered, visual,
                [visual] { QApplication::clipboard()->setText(visual->csv()); });
        auto *exportChart =
            AssistantControls::command(menu, "Save chart image…", chatIcon("download", theme_.text));
        connect(exportChart, &QAction::triggered, png, &QPushButton::click);
        actions->addStretch();
        content->addLayout(actions);
    }
    if (!message.sources.isEmpty()) {
        auto *group = new ChatGroup(
            QString("%1 source%2").arg(message.sources.size()).arg(message.sources.size() == 1 ? "" : "s"),
            bubble);
        auto *sources = new AssistantControls::VBoxLayout(group->body());
        sources->setContentsMargins(0, 3, 0, 0);
        for (qsizetype citation = 0; citation < message.sources.size(); ++citation) {
            const auto &source = message.sources.at(citation);
            const auto reference = QString("[%1] %2").arg(citation + 1).arg(source.location);
            auto *link = tool("file", reference, "Open the formatted excerpt for this citation", bubble);
            link->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            sources->addWidget(link);
            AssistantControls::onTrigger(link, this, [this, source, reference] {
                auto *d = new QDialog(this);
                d->setAttribute(Qt::WA_DeleteOnClose);
                d->setWindowTitle("Reference " + reference);
                d->resize(650, 480);
                auto *l = new AssistantControls::VBoxLayout(d);
                l->setContentsMargins(18, 16, 18, 16);
                auto *heading = new QLabel(d);
                heading->setTextFormat(Qt::PlainText);
                heading->setText(reference);
                heading->setObjectName("sectionTitle");
                heading->setWordWrap(true);
                l->addWidget(heading);
                auto *text = new ReferenceBrowser(d);
                text->setOpenLinks(false);
                text->setOpenExternalLinks(false);
                text->document()->setDefaultFont(QFont(font().family(), fontSize_->value()));
                text->document()->setDocumentMargin(10);
                if (source.kind == "document") {
                    text->document()->setMarkdown(readableTutorialMarkdown(source.content),
                                                  QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub) | QTextDocument::MarkdownNoHTML);
                } else {
                    text->setPlainText(source.content);
                }
                l->addWidget(text);
                auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
                l->addWidget(buttons);
                connect(buttons, &QDialogButtonBox::rejected, d, &QDialog::close);
                d->setPalette(palette());
                d->setStyleSheet(styleSheet());
                d->show();
            });
        }
        group->setExpanded(false);
        group->setProperty("animations", animations_->isChecked());
        content->addWidget(group);
    }
    line->addWidget(bubble, user ? 0 : 1);
    if (!user)
        line->addStretch(0);
    if (index == streamMessage_ && waiting_) {
        pendingReply_ = browser;
        pendingRow_ = outer;
        for (auto *b : bubble->findChildren<QPushButton *>())
            b->setEnabled(false);
    }
    return outer;
}

void ChatPanel::renderMessages() {
    const int previous = scroll_->verticalScrollBar()->value();
    messagesCanvas_->setUpdatesEnabled(false);
    pendingReply_.clear();
    pendingRow_.clear();
    while (auto *item = messagesLayout_->takeAt(0)) {
        if (item->widget())
            delete item->widget();
        delete item;
    }
    const int start = std::max(0, int(chat_.messages.size()) - renderLimit_);
    if (start > 0) {
        auto *earlier =
            tool("history", QString("Show %1 earlier messages").arg(start),
                 "Load older messages without changing the current conversation", messagesCanvas_);
        messagesLayout_->addWidget(AssistantControls::container(earlier));
        AssistantControls::onTrigger(earlier, this, [this] {
            renderLimit_ += messagePageSize_;
            followTail_ = false;
            renderMessages();
        });
    }
    if (chat_.messages.isEmpty()) {
        auto *welcome = new QFrame;
        welcome->setObjectName("assistantMessage");
        welcome->setMaximumWidth(chatWidth_->value());
        auto *l = new AssistantControls::VBoxLayout(welcome);
        l->setContentsMargins(24, 24, 24, 24);
        l->setSpacing(12);
        auto *title = new QLabel("Knowledge Assistant");
        title->setObjectName("sectionTitle");
        title->setWordWrap(true);
        l->addWidget(title);
        l->addWidget(caption("Ask a question or choose a documented workflow from the question library."));
        auto *buttons = new AssistantControls::HBoxLayout;
        auto *library =
            tool("chat", "Question library", "Browse or import grouped suggested questions", welcome);
        AssistantControls::onTrigger(library, this, [this] {
            sidebarButton_->setChecked(true);
            sideTabs_->setCurrentIndex(1);
        });
        buttons->addWidget(library);
        auto *settings = tool("settings", "Customize",
                              "Choose themes, answer format and interaction preferences", welcome);
        AssistantControls::onTrigger(settings, this, [this] { settingsButton_->setChecked(true); });
        buttons->addWidget(settings);
        buttons->addStretch();
        l->addLayout(buttons);
        messagesLayout_->addWidget(welcome);
    } else
        for (int i = start; i < chat_.messages.size(); ++i)
            messagesLayout_->addWidget(messageWidget(chat_.messages[i], i));
    messagesLayout_->addStretch(1);
    messagesCanvas_->setUpdatesEnabled(true);
    title_->setText(chat_.title);
    contextLabel_->setText(chat_.linkedContext.isEmpty() ? QString() : "Linked chat context");
    contextLabel_->setToolTip(chat_.linkedContext.isEmpty()
                                  ? QString()
                                  : "This conversation explicitly includes context from another chat. Use "
                                    "the conversation menu to change or remove it.");
    if (followTail_)
        QTimer::singleShot(0, this, &ChatPanel::followLatest);
    else
        QTimer::singleShot(0, this, [this, previous] {
            adjustingScroll_ = true;
            scroll_->verticalScrollBar()->setValue(previous);
            adjustingScroll_ = false;
        });
}
void ChatPanel::renderStreaming() {
    if (!waiting_ || !pendingReply_)
        return;
    const auto markdown = readableTutorialMarkdown(replyText_);
    pendingReply_->setProperty("assistantMarkdown", markdown);
    pendingReply_->setChatMarkdown(markdown.isEmpty() ? "*Preparing your answer…*" : markdown, theme_,
                                   fontSize_->value());
    if (pendingRow_)
        pendingRow_->setProperty("messageText", replyText_);
}
void ChatPanel::followLatest() {
    if (!followTail_ || !autoScroll_ || !autoScroll_->isChecked())
        return;
    adjustingScroll_ = true;
    auto *bar = scroll_->verticalScrollBar();
    bar->setValue(bar->maximum());
    adjustingScroll_ = false;
    AssistantControls::setVisible(latestButton_, false);
}

InferenceSettings ChatPanel::currentSettings() const {
    InferenceSettings s;
    s.contextTokens = contextTokens_->value();
    s.responseTokens = maxTokens_->value();
    s.topK = topK_->value();
    s.temperature = temperature_->value();
    s.repeatPenalty = repeatPenalty_->value();
    s.chatMl = chatMl_->isChecked();
    s.useKnowledge = includeSources_->isChecked();
    s.answerStyle = answerStyle_->currentText();
    s.instructions = customInstructions_->toPlainText().left(2000);
    return s;
}
QStringList ChatPanel::history() const {
    QStringList result;
    if (!chat_.linkedContext.isEmpty()) {
        auto linked = store_.load(chat_.linkedContext);
        if (linked.id.isEmpty())
            linked = memoryChats_.value(chat_.linkedContext);
        if (!linked.id.isEmpty()) {
            result << "Explicitly linked conversation: " + linked.title;
            for (const auto &m : linked.messages.mid(std::max(0, int(linked.messages.size()) - 4)))
                result << (m.role == "user" ? "User: " : "Assistant: ") + m.text.left(700);
        }
    }
    const int count = historyTurns_->value() * 2;
    for (const auto &m : chat_.messages.mid(std::max(0, int(chat_.messages.size()) - count)))
        if (!m.text.isEmpty())
            result << (m.role == "user" ? "User: " : "Assistant: ") + m.text.left(1400);
    int remaining = std::clamp((contextTokens_->value() - maxTokens_->value() - 1100) * 2, 400, 10000);
    QStringList bounded;
    for (int i = result.size() - 1; i >= 0 && remaining > 0; --i) {
        auto text = result[i].right(remaining);
        bounded.prepend(text);
        remaining -= text.size();
    }
    return bounded;
}
void ChatPanel::sendQuestion() {
    const QString question = input_->toPlainText().trimmed();
    if (question.isEmpty() || waiting_ || inference_.isBusy())
        return;
    if (!sendButton_->isEnabled()) {
        setStatus("Choose a model package and runtime in preferences", false);
        return;
    }
    if (editIndex_ >= 0) {
        const int index = editIndex_;
        cancelEdit();
        branchChat(index - 1);
    }
    const auto h = history();
    const auto settings = currentSettings();
    lastQuestion_ = question;
    input_->clear();
    drafts_.remove(chat_.id);
    prepareTimer_->stop();
    if (chat_.messages.isEmpty() && chat_.title == "New conversation")
        chat_.title = question.simplified().left(58);
    chat_.messages.append({newId(), "user", question, {}});
    activeSources_.clear();
    elapsed_.restart();
    firstTextMs_ = -1;
    metrics_ = {};
    stoppedReply_ = false;
    paused_ = false;
    updateProgress("search");
    const auto retrieval = retrieveSources(question, settings);
    activeSources_ = retrieval.hits;
    if (!retrieval.error.isEmpty()) {
#ifdef ASSISTANT_MANIVAULT_ACTIONS
        if (hostTask_)
            hostTask_->setAborted();
#endif
        chat_.messages.append({newId(), "assistant", "Local document search failed: " + retrieval.error, {}});
        saveChat();
        renderMessages();
        return;
    }
    if (KnowledgeIndex::isUserInterfaceQuestion(question) && settings.useKnowledge &&
        activeSources_.isEmpty()) {
#ifdef ASSISTANT_MANIVAULT_ACTIONS
        if (hostTask_)
            hostTask_->setFinished();
#endif
        chat_.messages.append(
            {newId(),
             "assistant",
             "I couldn't find a documented procedure for that question. Please name the feature or workflow, "
             "or choose a question from the library so I can retrieve the right guide.",
             {}});
        saveChat();
        renderMessages();
        return;
    }
    replyText_.clear();
    waiting_ = true;
    streamMessage_ = chat_.messages.size();
    chat_.messages.append({newId(), "assistant", QString(), activeSources_});
    followTail_ = autoScroll_->isChecked();
    renderMessages();
    saveChat();
    AssistantControls::setVisible(sendButton_, false);
    AssistantControls::setVisible(stopButton_, true);
    AssistantControls::setVisible(pauseButton_, inference_.supportsPause());
    pauseButton_->setText("Pause");
    pauseButton_->setProperty("iconName", "pause");
    pauseButton_->setIcon(chatIcon("pause", theme_.text));
    if (auto *action = AssistantControls::action(pauseButton_)) {
        action->setText("Pause");
        action->setIcon(pauseButton_->icon());
    }
    setStatus("Writing · " + inference_.runtimeDescription(), true);
    updateProgress("context");
    inference_.ask(question, activeSources_, h, settings);
}
void ChatPanel::finishReply(const QString &error) {
    AssistantControls::setEnabled(sendButton_, index_.isOpen() &&
                                                   !RuntimePaths::modelFile(packagePath_).isEmpty() &&
                                                   QFileInfo(runtimePath_).isExecutable());
    if (!waiting_) {
        progress_->setRange(0, 100);
        progress_->setValue(0);
        progressLabel_->setText(error.isEmpty() ? "Ready" : "Runtime error");
        statisticsLabel_->clear();
        setStatus(error.isEmpty() ? "Ready · " + inference_.runtimeDescription() : "Runtime error · " + error,
                  error.isEmpty());
        if (prepareTimer_ && !input_->toPlainText().isEmpty())
            prepareTimer_->start();
        return;
    }
    renderTimer_->stop();
    waiting_ = false;
    paused_ = false;
    if (streamMessage_ >= 0 && streamMessage_ < chat_.messages.size())
        chat_.messages[streamMessage_].text =
            !error.isEmpty()       ? "**Response error:** " + error
            : replyText_.isEmpty() ? (stoppedReply_ ? "*Response stopped.*" : "*No answer was produced.*")
                                   : replyText_;
    streamMessage_ = -1;
    AssistantControls::setVisible(sendButton_, true);
    AssistantControls::setVisible(stopButton_, false);
    AssistantControls::setVisible(pauseButton_, false);
    progress_->setRange(0, 100);
    progress_->setValue(100);
    progressLabel_->setText(stoppedReply_ ? "Stopped" : error.isEmpty() ? "Complete" : "Response error");
    QString stats = QString("%1 s").arg(elapsed_.elapsed() / 1000.0, 0, 'f', 2);
    if (firstTextMs_ >= 0)
        stats += QString(" · first text %1 ms").arg(firstTextMs_);
    const double generation = metrics_.value("generation_ms").toDouble();
    if (generation > 0)
        stats += QString(" · %1 tokens/s")
                     .arg(metrics_.value("generated_tokens").toInt() * 1000.0 / generation, 0, 'f', 0);
    statisticsLabel_->setText(stats);
#ifdef ASSISTANT_MANIVAULT_ACTIONS
    if (hostTask_) {
        if (stoppedReply_ || !error.isEmpty())
            hostTask_->setAborted();
        else
            hostTask_->setFinished();
    }
#endif
    chatTip(
        statisticsLabel_, "Response performance",
        stats + "\n" +
            QString(
                "Prompt: %1 tokens; reused: %2; generated: %3. Context processing: %4 ms; generation: %5 ms.")
                .arg(metrics_.value("prompt_tokens").toInt())
                .arg(metrics_.value("reused_tokens").toInt())
                .arg(metrics_.value("generated_tokens").toInt())
                .arg(metrics_.value("prefill_ms").toDouble(), 0, 'f', 0)
                .arg(generation, 0, 'f', 0));
    saveChat();
    // Replace only the completed bubble; rebuilding the transcript resets its
    // scroll range and visibly jumps to the top during fast token streaming.
    auto *old = pendingRow_.data();
    if (old) {
        const int position = messagesLayout_->indexOf(old);
        const int anchor = scroll_->verticalScrollBar()->value();
        scroll_->viewport()->setUpdatesEnabled(false);
        messagesLayout_->removeWidget(old);
        old->hide();
        auto *row = messageWidget(chat_.messages.last(), chat_.messages.size() - 1);
        messagesLayout_->insertWidget(std::max(0, position), row);
        old->deleteLater();
        messagesLayout_->activate();
        QTimer::singleShot(0, this, [this, anchor] {
            if (followTail_)
                followLatest();
            else
                scroll_->verticalScrollBar()->setValue(anchor);
            scroll_->viewport()->setUpdatesEnabled(true);
        });
    } else
        renderMessages();
    pendingReply_.clear();
    pendingRow_.clear();
    setStatus(error.isEmpty() ? "Ready · " + inference_.runtimeDescription() : "Response error · See message",
              error.isEmpty());
}
void ChatPanel::cancelForNavigation() {
    if (!waiting_)
        return;
    if (streamMessage_ >= 0 && streamMessage_ < chat_.messages.size())
        chat_.messages[streamMessage_].text = replyText_.isEmpty() ? "*Response stopped.*" : replyText_;
    waiting_ = false;
    streamMessage_ = -1;
    if (renderTimer_)
        renderTimer_->stop();
    pendingReply_.clear();
    pendingRow_.clear();
    inference_.stop();
#ifdef ASSISTANT_MANIVAULT_ACTIONS
    if (hostTask_)
        hostTask_->setAborted();
#endif
    AssistantControls::setVisible(sendButton_, true);
    AssistantControls::setEnabled(sendButton_, false);
    AssistantControls::setVisible(stopButton_, false);
    AssistantControls::setVisible(pauseButton_, false);
    saveChat();
}

void ChatPanel::saveChat() {
    if (chat_.id.isEmpty())
        return;
    if (saveHistory_ && saveHistory_->isChecked()) {
        QString error;
        if (!store_.save(chat_, &error)) {
            setStatus("History could not be saved: " + error, false);
            memoryChats_[chat_.id] = chat_;
        } else
            memoryChats_.remove(chat_.id);
    } else
        memoryChats_[chat_.id] = chat_;
    refreshChats();
}
void ChatPanel::refreshChats() {
    if (!chats_ || refreshingChats_)
        return;
    QScopedValueRollback<bool> refreshing(refreshingChats_, true);
    const int previousScroll = chats_->verticalScrollBar()->value();
    const QSignalBlocker block(chats_);
    summaries_ = saveHistory_ && saveHistory_->isChecked() ? store_.list() : QList<ConversationSummary>{};
    QSet<QString> known;
    for (const auto &s : summaries_)
        known.insert(s.id);
    for (auto it = memoryChats_.begin(); it != memoryChats_.end(); ++it)
        if (!known.contains(it.key()))
            summaries_.prepend({it.key(), it->title,
                                it->messages.isEmpty() ? QString() : it->messages.last().text.left(150),
                                it->pinned, it->group});
    QStringList groups;
    for (const auto &summary : summaries_)
        if (!summary.group.isEmpty() && !groups.contains(summary.group))
            groups << summary.group;
    groups.sort(Qt::CaseInsensitive);
    QStringList options{"All groups", "Pinned only", "Ungrouped"};
    for (const auto &group : groups)
        options << "Group: " + group;
    const auto selected = pendingGroupFilter_.isEmpty() ? groupFilter_->currentText() : pendingGroupFilter_;
    pendingGroupFilter_.clear();
    AssistantControls::setOptions(groupFilter_, options, selected);
    const int order = chatOrder_->currentIndex();
    std::stable_sort(summaries_.begin(), summaries_.end(), [order](const auto &a, const auto &b) {
        if (a.pinned != b.pinned)
            return a.pinned;
        if (order == 2 && a.group.compare(b.group, Qt::CaseInsensitive) != 0)
            return a.group.compare(b.group, Qt::CaseInsensitive) < 0;
        if (order > 0)
            return a.title.compare(b.title, Qt::CaseInsensitive) < 0;
        return false;
    });
    chats_->clear();
    const auto filter = chatSearch_->text().trimmed();
    for (const auto &s : summaries_) {
        const auto selectedGroup = groupFilter_->currentText();
        if ((selectedGroup == "Pinned only" && !s.pinned) ||
            (selectedGroup == "Ungrouped" && !s.group.isEmpty()) ||
            (selectedGroup.startsWith("Group: ") && s.group != selectedGroup.mid(7)))
            continue;
        if (!filter.isEmpty() && !s.title.contains(filter, Qt::CaseInsensitive) &&
            !s.preview.contains(filter, Qt::CaseInsensitive) &&
            !s.group.contains(filter, Qt::CaseInsensitive))
            continue;
        auto *item = new QListWidgetItem(chatIcon(s.pinned ? "star" : "chat", theme_.muted),
                                         s.title + (s.group.isEmpty() ? QString() : "\n" + s.group), chats_);
        item->setData(Qt::UserRole, s.id);
        item->setData(Qt::UserRole + 1, s.pinned);
        item->setToolTip((s.group.isEmpty() ? QString() : "Group: " + s.group + "\n") + s.preview);
        if (s.id == chat_.id)
            chats_->setCurrentItem(item);
    }
    chats_->verticalScrollBar()->setValue(previousScroll);
}
void ChatPanel::newChat() {
    cancelEdit();
    if (!chat_.id.isEmpty())
        drafts_[chat_.id] = input_->toPlainText();
    cancelForNavigation();
    if (!chat_.id.isEmpty())
        saveChat();
    chat_ = Conversation{};
    chat_.id = newId();
    if (groupFilter_->currentText().startsWith("Group: "))
        chat_.group = groupFilter_->currentText().mid(7);
    if (groupFilter_->currentText() == "Pinned only")
        AssistantControls::select(groupFilter_, "All groups");
    renderLimit_ = messagePageSize_;
    followTail_ = true;
    input_->clear();
    messageSearch_->clear();
    saveChat();
    renderMessages();
}
void ChatPanel::switchChat(const QString &id) {
    if (id.isEmpty() || id == chat_.id)
        return;
    cancelEdit();
    if (!chat_.id.isEmpty())
        drafts_[chat_.id] = input_->toPlainText();
    cancelForNavigation();
    if (!chat_.id.isEmpty())
        saveChat();
    auto next = store_.load(id);
    if (next.id.isEmpty())
        next = memoryChats_.value(id);
    if (next.id.isEmpty())
        return;
    chat_ = next;
    renderLimit_ = messagePageSize_;
    followTail_ = true;
    input_->setPlainText(drafts_.value(id));
    messageSearch_->clear();
    renderMessages();
    refreshChats();
}
void ChatPanel::saveConversation(Conversation &target) {
    if (target.id == chat_.id) {
        chat_ = target;
        saveChat();
        target = chat_;
        title_->setText(chat_.title);
        return;
    }
    const auto originalId = target.id;
    QString error;
    if (saveHistory_->isChecked() && store_.save(target, &error))
        memoryChats_.remove(originalId);
    else {
        memoryChats_[target.id] = target;
        if (!error.isEmpty())
            setStatus("Changes kept in memory; history save failed: " + error, false);
    }
    refreshChats();
}
void ChatPanel::renameChat(const QString &id) {
    const auto targetId = id.isEmpty() ? chat_.id : id;
    auto target = targetId == chat_.id              ? chat_
                  : memoryChats_.contains(targetId) ? memoryChats_.value(targetId)
                                                    : store_.load(targetId);
    if (target.id.isEmpty())
        return;
    bool ok = false;
    auto title =
        QInputDialog::getText(this, "Rename conversation", "Title", QLineEdit::Normal, target.title, &ok)
            .simplified();
    if (ok && !title.isEmpty()) {
        target = targetId == chat_.id              ? chat_
                 : memoryChats_.contains(targetId) ? memoryChats_.value(targetId)
                                                   : store_.load(targetId);
        if (target.id.isEmpty())
            return;
        if (target.title == title.left(120))
            return;
        target.title = title.left(120);
        saveConversation(target);
    }
}
void ChatPanel::groupChat(const QString &id) {
    const auto targetId = id.isEmpty() ? chat_.id : id;
    auto target = targetId == chat_.id              ? chat_
                  : memoryChats_.contains(targetId) ? memoryChats_.value(targetId)
                                                    : store_.load(targetId);
    if (target.id.isEmpty())
        return;
    QStringList names;
    for (const auto &summary : summaries_)
        if (!summary.group.isEmpty() && !names.contains(summary.group))
            names << summary.group;
    names.sort(Qt::CaseInsensitive);
    if (!names.contains(target.group))
        names.prepend(target.group);
    bool ok = false;
    const auto chosen = QInputDialog::getItem(this, "Move conversation to group",
                                              "Choose or type a group name. Leave empty to remove grouping.",
                                              names, std::max(0, int(names.indexOf(target.group))), true, &ok)
                            .simplified()
                            .left(80);
    if (!ok)
        return;
    target = targetId == chat_.id              ? chat_
             : memoryChats_.contains(targetId) ? memoryChats_.value(targetId)
                                               : store_.load(targetId);
    if (target.id.isEmpty())
        return;
    if (target.group == chosen)
        return;
    target.group = chosen;
    saveConversation(target);
}
void ChatPanel::showChatMenu(const QPoint &position) {
    const auto *item = chats_->itemAt(position);
    const QString id = item ? item->data(Qt::UserRole).toString() : QString();
    bool pinned = id == chat_.id && chat_.pinned;
    QString selectedGroup = id == chat_.id ? chat_.group : QString();
    for (const auto &summary : summaries_)
        if (summary.id == id) {
            pinned = summary.pinned;
            selectedGroup = summary.group;
        }
    auto *menu = new QMenu(chats_);
    menu->setObjectName("chatContextMenu");
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    auto add = [this, menu](const QString &title, const QString &icon, std::function<void()> callback) {
        auto *action = AssistantControls::command(menu, title, chatIcon(icon, theme_.text));
        connect(action, &QAction::triggered, this, [menu, callback] {
            menu->hide();
            callback();
        });
        return action;
    };
    if (!id.isEmpty()) {
        add("Open conversation", "chat", [this, id] { switchChat(id); });
        add("Rename…", "file", [this, id] { renameChat(id); });
        add("Move to group…", "file", [this, id] { groupChat(id); });
        auto *removeGroup = add("Remove from group", "close", [this, id] {
            auto target = id == chat_.id              ? chat_
                          : memoryChats_.contains(id) ? memoryChats_.value(id)
                                                      : store_.load(id);
            if (!target.id.isEmpty()) {
                target.group.clear();
                saveConversation(target);
            }
        });
        removeGroup->setEnabled(!selectedGroup.isEmpty());
        add(pinned ? "Unpin conversation" : "Pin conversation", "star", [this, id] {
            auto target = id == chat_.id              ? chat_
                          : memoryChats_.contains(id) ? memoryChats_.value(id)
                                                      : store_.load(id);
            if (target.id.isEmpty())
                return;
            target.pinned = !target.pinned;
            saveConversation(target);
        });
        add("Branch conversation", "branch", [this, id] {
            switchChat(id);
            if (chat_.id == id)
                branchChat(chat_.messages.size() - 1);
        });
        add("Copy conversation", "copy", [this, id] {
            const auto target = id == chat_.id              ? chat_
                                : memoryChats_.contains(id) ? memoryChats_.value(id)
                                                            : store_.load(id);
            if (!target.id.isEmpty())
                QApplication::clipboard()->setText(plainChat(target));
        });
        add("Export conversation…", "download", [this, id] {
            switchChat(id);
            if (chat_.id == id)
                exportChat();
        });
        menu->addSeparator();
        add("Delete conversation…", "trash", [this, id] { deleteChat(id); });
    } else {
        add("New conversation", "plus", [this] { newChat(); });
        add("Import conversation…", "file", [this] { importChat(); });
        add("Search commands…", "search", [this] { showCommands(); });
    }
    menu->popup(chats_->viewport()->mapToGlobal(position));
}
void ChatPanel::deleteChat(const QString &id) {
    const auto targetId = id.isEmpty() ? chat_.id : id;
    if (targetId.isEmpty())
        return;
    QString title = chat_.title;
    for (const auto &summary : summaries_)
        if (summary.id == targetId)
            title = summary.title;
    if (QMessageBox::question(this, "Delete conversation",
                              QString("Delete “%1” and its messages?").arg(title),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    if (targetId == chat_.id)
        cancelForNavigation();
    QString error;
    if (!store_.open(&error) || !store_.remove(targetId, &error)) {
        setStatus("Could not delete conversation: " + error, false);
        return;
    }
    memoryChats_.remove(targetId);
    drafts_.remove(targetId);
    if (targetId == chat_.id) {
        chat_ = Conversation{};
        newChat();
    } else
        refreshChats();
}
void ChatPanel::deleteAllChats() {
    if (QMessageBox::question(this, "Delete saved conversations",
                              "Delete all saved conversations? This cannot be undone.",
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    cancelForNavigation();
    for (const auto &c : store_.list())
        store_.remove(c.id);
    memoryChats_.clear();
    drafts_.clear();
    chat_ = Conversation{};
    newChat();
}
void ChatPanel::branchChat(int index) {
    cancelEdit();
    drafts_[chat_.id] = input_->toPlainText();
    cancelForNavigation();
    saveChat();
    auto branch = chat_;
    branch.id = newId();
    branch.revision = 0;
    branch.title = "Branch · " + chat_.title;
    branch.pinned = false;
    branch.messages = chat_.messages.mid(0, std::clamp(index + 1, 0, int(chat_.messages.size())));
    chat_ = branch;
    input_->clear();
    followTail_ = true;
    saveChat();
    renderMessages();
    input_->setFocus();
}
void ChatPanel::deleteMessage(int index, bool following) {
    if (index < 0 || index >= chat_.messages.size())
        return;
    if (following &&
        QMessageBox::question(this, "Delete messages", "Delete this message and everything after it?",
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    cancelForNavigation();
    if (following)
        chat_.messages = chat_.messages.mid(0, index);
    else
        chat_.messages.removeAt(index);
    saveChat();
    renderMessages();
}
void ChatPanel::useOtherContext() {
    QStringList names{"None — keep this chat independent"};
    QStringList ids{QString()};
    for (const auto &c : summaries_)
        if (c.id != chat_.id) {
            names << c.title;
            ids << c.id;
        }
    bool ok = false;
    auto selected = QInputDialog::getItem(this, "Linked chat context",
                                          "Explicitly include recent messages from", names, 0, false, &ok);
    if (ok) {
        chat_.linkedContext = ids.value(names.indexOf(selected));
        saveChat();
        renderMessages();
    }
}
void ChatPanel::copyChat() {
    QApplication::clipboard()->setText(plainChat(chat_));
}
void ChatPanel::exportChat() {
    auto path = QFileDialog::getSaveFileName(this, "Export conversation", "assistant-chat.md",
                                             "Markdown (*.md);;Conversation JSON (*.json)");
    if (path.isEmpty())
        return;
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, "Export failed", f.errorString());
        return;
    }
    f.write(path.endsWith(".json", Qt::CaseInsensitive) ? QJsonDocument(ChatStore::toJson(chat_)).toJson()
                                                        : plainChat(chat_).toUtf8());
    if (!f.commit())
        QMessageBox::warning(this, "Export failed", f.errorString());
}
void ChatPanel::importChat() {
    auto path = QFileDialog::getOpenFileName(this, "Import conversation", {}, "Conversation JSON (*.json)");
    if (path.isEmpty())
        return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > 20 * 1024 * 1024) {
        QMessageBox::warning(this, "Import failed",
                             "Choose a valid conversation JSON file smaller than 20 MB.");
        return;
    }
    QJsonParseError error;
    const auto object = QJsonDocument::fromJson(f.readAll(), &error).object();
    if (error.error != QJsonParseError::NoError || !object.contains("messages")) {
        QMessageBox::warning(this, "Import failed", "This file is not a conversation export.");
        return;
    }
    cancelForNavigation();
    saveChat();
    chat_ = ChatStore::fromJson(object);
    chat_.id = newId();
    chat_.revision = 0;
    chat_.linkedContext.clear();
    saveChat();
    renderMessages();
}
void ChatPanel::regenerate(int index) {
    if (waiting_ || inference_.isBusy())
        return;
    if (index < 0)
        index = chat_.messages.size() - 1;
    while (index >= 0 && chat_.messages[index].role != "user")
        --index;
    if (index < 0)
        return;
    const auto prompt = chat_.messages[index].text;
    branchChat(index - 1);
    input_->setPlainText(prompt);
    sendQuestion();
}

void ChatPanel::refreshSuggestions() {
    while (auto *item = suggestionsLayout_->takeAt(0)) {
        if (item->widget())
            delete item->widget();
        delete item;
    }
    if (suggestionActions_) {
        delete suggestionActions_;
        suggestionActions_ = nullptr;
    }
#ifdef ASSISTANT_MANIVAULT_ACTIONS
    auto *actions = new mv::gui::GroupAction(this, "Suggested questions", false);
    suggestionActions_ = actions;
    auto *importAction = new mv::gui::TriggerAction(actions, "Import question file");
    actions->addAction(importAction);
    connect(importAction, &QAction::triggered, this, &ChatPanel::chooseSuggestions);
    auto *exportAction = new mv::gui::TriggerAction(actions, "Export question file");
    actions->addAction(exportAction);
    connect(exportAction, &QAction::triggered, this, &ChatPanel::exportSuggestions);
#endif
    auto *fileGroup = new ChatGroup("Question file", suggestionsCanvas_);
    fileGroup->setProperty("animations", animations_->isChecked());
    auto *files = new AssistantControls::VBoxLayout(fileGroup->body());
    files->setContentsMargins(0, 3, 0, 4);
    suggestionsFile_ = AssistantControls::line(this, "Question file", fileGroup);
    suggestionsFile_->setText(suggestionsPath_);
    suggestionsFile_->setReadOnly(true);
    suggestionsFile_->setPlaceholderText("Built-in question library");
    files->addWidget(suggestionsFile_);
    auto *fileActions = new AssistantControls::HBoxLayout;
    auto *choose = tool("file", "Import…", "Load grouped questions from JSON or a text file", fileGroup);
    AssistantControls::onTrigger(choose, this, [this] { chooseSuggestions(); });
    fileActions->addWidget(choose);
    auto *reload = tool("retry", "", "Reload the selected question file", fileGroup);
    AssistantControls::onTrigger(reload, this, [this] {
        if (suggestionsPath_.isEmpty()) {
            suggestions_ = defaultSuggestions();
            refreshSuggestions();
        } else
            loadSuggestions(suggestionsPath_);
    });
    fileActions->addWidget(reload);
    auto *exportFile = tool("download", "", "Export the current question library as JSON", fileGroup);
    AssistantControls::onTrigger(exportFile, this, [this] { exportSuggestions(); });
    fileActions->addWidget(exportFile);
    files->addLayout(fileActions);
    auto *builtIn = tool("chat", "Use built-in examples",
                         "Switch back to the bundled question library. Your imported file stays unchanged; "
                         "export first to keep unsaved custom questions.",
                         fileGroup);
    files->addWidget(builtIn);
    AssistantControls::onTrigger(builtIn, this, [this] {
        suggestionsPath_.clear();
        suggestions_ = defaultSuggestions();
        saveSettings();
        refreshSuggestions();
        applyTheme();
    });
    suggestionsLayout_->addWidget(fileGroup);
#ifdef ASSISTANT_MANIVAULT_ACTIONS
    connect(fileGroup->header(), &QToolButton::toggled, actions, &mv::gui::GroupAction::setExpanded);
#endif
    auto *questionSearch = AssistantControls::line(this, "Search suggested questions", suggestionsCanvas_);
    questionSearch->setObjectName("questionSearch");
    questionSearch->setPlaceholderText("Search projects, plugins or questions…");
    questionSearch->setClearButtonEnabled(true);
    chatTip(questionSearch, "Search the question library",
            "Search across categories, subcategories and full question text. Click a question to edit it "
            "before sending.");
    suggestionsLayout_->addWidget(AssistantControls::container(questionSearch));
    std::function<void(const QJsonObject &, QWidget *, QVBoxLayout *, QObject *, int)> addGroup;
    int groupCount = 0, questionCount = 0;
    addGroup = [&](const QJsonObject &object, QWidget *parent, QVBoxLayout *target, QObject *owner,
                   int depth) {
        if (depth > 5 || ++groupCount > 100)
            return;
        const auto title = object["title"].toString("Questions");
        auto *group = new ChatGroup(title, parent);
        group->setProperty("suggestionGroup", true);
        group->setProperty("defaultExpanded", depth == 0);
        QString searchable = title;
        auto *items = new AssistantControls::VBoxLayout(group->body());
        items->setContentsMargins(0, 0, 0, 3);
        items->setSpacing(5);
        group->setProperty("animations", animations_->isChecked());
#ifdef ASSISTANT_MANIVAULT_ACTIONS
        auto *actionGroup = new mv::gui::GroupAction(owner, title, depth == 0);
        if (auto *parentAction = qobject_cast<mv::gui::GroupAction *>(owner))
            parentAction->addAction(actionGroup);
        connect(group->header(), &QToolButton::toggled, actionGroup, &mv::gui::GroupAction::setExpanded);
        connect(actionGroup, &mv::gui::GroupAction::expanded, group, [group] {
            if (!group->isExpanded())
                group->setExpanded(true, group->property("animations").toBool());
        });
        connect(actionGroup, &mv::gui::GroupAction::collapsed, group, [group] {
            if (group->isExpanded())
                group->setExpanded(false, group->property("animations").toBool());
        });
        owner = actionGroup;
#endif
        for (const auto &question : object["questions"].toArray()) {
            if (++questionCount > 300)
                break;
            const auto o = question.toObject();
            const auto prompt = question.isString() ? question.toString() : o["prompt"].toString();
            const auto label = question.isString() ? prompt : o["label"].toString(prompt);
            if (prompt.trimmed().isEmpty())
                continue;
            searchable += ' ' + label + ' ' + prompt;
            auto *item = tool("chat", label.left(48), prompt, group);
            item->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            item->setMinimumWidth(0);
            item->setProperty("assistantQuestionLabel", label);
            item->installEventFilter(this);
            items->addWidget(item);
            auto use = [this, prompt] {
                input_->setPlainText(prompt);
                input_->setFocus();
                input_->moveCursor(QTextCursor::End);
            };
#ifdef ASSISTANT_MANIVAULT_ACTIONS
            auto *action = qobject_cast<mv::gui::TriggerAction *>(AssistantControls::action(item));
            action->setParent(actionGroup);
            actionGroup->addAction(action);
            connect(action, &QAction::triggered, this, use);
#else
            AssistantControls::onTrigger(item, this, use);
#endif
        }
        for (const auto &child : object["groups"].toArray())
            addGroup(child.toObject(), group->body(), items, owner, depth + 1);
        for (auto *child : group->body()->findChildren<ChatGroup *>(QString(), Qt::FindDirectChildrenOnly))
            searchable += ' ' + child->property("questionSearchText").toString();
        group->setProperty("questionSearchText", searchable);
        group->setExpanded(depth == 0);
        target->addWidget(group);
    };
    for (const auto &value : suggestions_["groups"].toArray())
        addGroup(value.toObject(), suggestionsCanvas_, suggestionsLayout_, suggestionActions_, 0);
    connect(questionSearch, &QLineEdit::textChanged, this, [this](const QString &term) {
        for (auto *group : suggestionsCanvas_->findChildren<ChatGroup *>()) {
            if (!group->property("suggestionGroup").toBool())
                continue;
            group->setVisible(term.isEmpty() || group->property("questionSearchText")
                                                    .toString()
                                                    .contains(term.trimmed(), Qt::CaseInsensitive));
            group->setExpanded(term.isEmpty() ? group->property("defaultExpanded").toBool() : true);
        }
    });
    auto *add = tool("plus", "Add a question…", "Create a custom suggested question", suggestionsCanvas_);
    AssistantControls::onTrigger(add, this, [this] { addSuggestion(); });
    suggestionsLayout_->addWidget(AssistantControls::container(add));
    suggestionsLayout_->addStretch();
}
void ChatPanel::chooseSuggestions() {
    auto path = QFileDialog::getOpenFileName(this, "Suggested questions", {},
                                             "Question libraries (*.json *.txt *.md);;All files (*)");
    if (!path.isEmpty())
        loadSuggestions(path);
}
void ChatPanel::loadSuggestions(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > 1024 * 1024) {
        setStatus("Cannot load question file · Use a file smaller than 1 MB", false);
        return;
    }
    const auto bytes = f.readAll();
    QJsonObject parsed;
    if (path.endsWith(".json", Qt::CaseInsensitive)) {
        QJsonParseError error;
        parsed = QJsonDocument::fromJson(bytes, &error).object();
        if (error.error != QJsonParseError::NoError || !parsed["groups"].isArray()) {
            setStatus("Question file needs a JSON groups array", false);
            return;
        }
    } else {
        QJsonArray groups, questions;
        QString title = "Questions";
        for (auto line : QString::fromUtf8(bytes).split('\n')) {
            line = line.trimmed();
            if (line.isEmpty() || line.startsWith('#'))
                continue;
            if (line.startsWith('[') && line.endsWith(']')) {
                if (!questions.isEmpty())
                    groups.append(QJsonObject{{"title", title}, {"questions", questions}});
                title = line.mid(1, line.size() - 2);
                questions = {};
            } else
                questions.append(line);
        }
        if (!questions.isEmpty())
            groups.append(QJsonObject{{"title", title}, {"questions", questions}});
        parsed = {{"version", 1}, {"groups", groups}};
    }
    const auto groups = parsed["groups"].toArray();
    int groupCount = 0, questionCount = 0;
    if (groups.isEmpty() || !validQuestionGroups(groups, groupCount, questionCount) || questionCount == 0) {
        setStatus("Use named categories up to 6 levels, 100 groups and 300 questions; prompts need 1–5000 "
                  "characters",
                  false);
        return;
    }
    suggestions_ = parsed;
    suggestionsPath_ = path;
    saveSettings();
    refreshSuggestions();
    applyTheme();
}
void ChatPanel::exportSuggestions() {
    auto path = QFileDialog::getSaveFileName(this, "Export question library", "suggested-questions.json",
                                             "JSON (*.json)");
    if (path.isEmpty())
        return;
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        setStatus("Could not export question library", false);
        return;
    }
    f.write(QJsonDocument(suggestions_).toJson());
    if (!f.commit())
        setStatus("Could not save question library", false);
}
void ChatPanel::addSuggestion() {
    bool ok = false;
    auto prompt =
        QInputDialog::getMultiLineText(this, "Add suggested question", "Question or prompt", {}, &ok)
            .trimmed();
    if (!ok || prompt.isEmpty())
        return;
    auto groups = suggestions_["groups"].toArray();
    int index = -1;
    for (int i = 0; i < groups.size(); ++i)
        if (groups[i].toObject()["title"].toString() == "Custom")
            index = i;
    auto group =
        index >= 0 ? groups[index].toObject() : QJsonObject{{"title", "Custom"}, {"questions", QJsonArray()}};
    auto questions = group["questions"].toArray();
    questions.append(QJsonObject{{"label", prompt.simplified().left(45)}, {"prompt", prompt.left(5000)}});
    group["questions"] = questions;
    if (index >= 0)
        groups[index] = group;
    else
        groups.append(group);
    suggestions_["groups"] = groups;
    refreshSuggestions();
    applyTheme();
    setStatus("Question added · Export the library to keep your custom questions", true);
}

bool ChatPanel::eventFilter(QObject *watched, QEvent *event) {
    if (watched == chats_ && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_F2 && chats_->currentItem()) {
            if (!key->isAutoRepeat())
                renameChat(chats_->currentItem()->data(Qt::UserRole).toString());
            return true;
        }
        if (key->key() == Qt::Key_Delete && chats_->currentItem()) {
            if (!key->isAutoRepeat())
                deleteChat(chats_->currentItem()->data(Qt::UserRole).toString());
            return true;
        }
    }
    if (!watched->property("assistantQuestionLabel").toString().isEmpty() &&
        event->type() == QEvent::Resize) {
        QPointer<QPushButton> button = qobject_cast<QPushButton *>(watched);
        QTimer::singleShot(0, this, [button] {
            if (button)
                fitQuestionButton(button);
        });
    }
    if (watched->property("assistantActionLabel").toBool() &&
        (event->type() == QEvent::Enter || event->type() == QEvent::Leave ||
         event->type() == QEvent::EnabledChange)) {
        QPointer<QLabel> label = qobject_cast<QLabel *>(watched);
        QTimer::singleShot(0, this, [this, label] {
            if (label)
                label->setStyleSheet(QString("QLabel { color:%1; background:transparent; }")
                                         .arg((label->isEnabled() ? theme_.text : theme_.muted).name()));
        });
    }
    if (watched == input_ && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        const bool enter = key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter;
        if (enter && key->isAutoRepeat())
            return true;
        if (enter && !key->isAutoRepeat()) {
            if (key->modifiers() & Qt::ShiftModifier)
                return false;
            const bool command = key->modifiers() & (Qt::ControlModifier | Qt::MetaModifier);
            if (command || enterMode_->currentIndex() == 0) {
                sendQuestion();
                return true;
            }
        }
    }
    if (event->type() == QEvent::Wheel && scroll_ &&
        (watched == scroll_->viewport() || watched == scroll_->verticalScrollBar() ||
         watched->property("chatMessageViewport").toBool())) {
        const auto *wheel = static_cast<QWheelEvent *>(event);
        if (wheel->angleDelta().y() > 0) {
            followTail_ = false;
            AssistantControls::setVisible(latestButton_, true);
        } else
            QTimer::singleShot(0, this, [this] {
                auto *s = scroll_->verticalScrollBar();
                if (s->maximum() - s->value() < 24) {
                    followTail_ = true;
                    AssistantControls::setVisible(latestButton_, false);
                }
            });
    }
    return QWidget::eventFilter(watched, event);
}
