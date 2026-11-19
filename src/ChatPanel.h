#pragma once
#include "ChatStore.h"
#include "ChatUi.h"
#include "OfflineInference.h"
#include <QElapsedTimer>
#include <QMap>
#include <QPointer>
#include <QWidget>

class QCheckBox;
class QAction;
class QComboBox;
class QDialog;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QSplitter;
class QTabWidget;
class QTextEdit;
class QTimer;
class QVBoxLayout;
namespace mv {
class Task;
}

class ChatPanel final : public QWidget {
    Q_OBJECT
    friend class ChatPanelTest;

  public:
    explicit ChatPanel(QWidget *parent = nullptr, bool initializeRuntime = true);
    ~ChatPanel() override;

  protected:
    bool eventFilter(QObject *, QEvent *) override;
    void resizeEvent(QResizeEvent *) override;

  private:
    void buildUi();
    void buildSettings();
    void loadSettings();
    void saveSettings();
    void applyTheme();
    void refreshPaths();
    void setStatus(const QString &, bool);
    void toggleSettings(bool);
    void sendQuestion();
    void finishReply(const QString &);
    void renderMessages();
    QWidget *messageWidget(const ChatMessage &, int index);
    void refreshMessagesAppearance();
    void renderStreaming();
    void followLatest();
    void updateProgress(const QString &, int current = 0, int total = 0);
    void showCommands();
    void copyDiagnostics();
    void editMessage(int index);
    void editSavedMessage(int index);
    void cancelEdit();
    void refreshChats();
    void newChat();
    void switchChat(const QString &);
    void saveChat();
    void renameChat(const QString &id = {});
    void groupChat(const QString &id = {});
    void saveConversation(Conversation &conversation);
    void deleteChat(const QString &id = {});
    void showChatMenu(const QPoint &position);
    void deleteAllChats();
    void branchChat(int index);
    void deleteMessage(int index, bool following);
    void useOtherContext();
    void copyChat();
    void exportChat();
    void importChat();
    void regenerate(int index = -1);
    void cancelForNavigation();
    void refreshSuggestions();
    void chooseSuggestions();
    void exportSuggestions();
    void addSuggestion();
    void loadSuggestions(const QString &path);
    QStringList history() const;
    InferenceSettings currentSettings() const;
    QPushButton *tool(const QString &icon, const QString &title, const QString &tip,
                      QWidget *parent = nullptr);

    KnowledgeIndex index_;
    OfflineInference inference_;
    ChatStore store_;
    Conversation chat_;
    QMap<QString, Conversation> memoryChats_;
    QMap<QString, QString> drafts_;
    QList<QPointer<QAction>> commands_;
    mv::Task *hostTask_ = nullptr;
    QList<ConversationSummary> summaries_;
    QList<SourceHit> activeSources_;
    ChatTheme theme_;
    QColor accent_;
    QString packagePath_, runtimePath_, suggestionsPath_, replyText_, lastQuestion_;
    QString pendingGroupFilter_;
    QJsonObject suggestions_;
    QStringList messageTools_{"Copy", "Branch", "Bookmark"};
    QObject *suggestionActions_ = nullptr;
    bool waiting_ = false, paused_ = false, followTail_ = true, adjustingScroll_ = false,
         loadingSettings_ = true, navigating_ = false, stoppedReply_ = false, refreshingChats_ = false;
    int renderLimit_ = 80, streamMessage_ = -1, editIndex_ = -1;
    QElapsedTimer elapsed_;
    qint64 firstTextMs_ = -1;
    QJsonObject metrics_;
    QSplitter *splitter_ = nullptr;
    QWidget *sidebar_ = nullptr;
    QWidget *chatColumn_ = nullptr;
    QFrame *settingsPanel_ = nullptr;
    QDialog *settingsDialog_ = nullptr;
    QScrollArea *settingsScroll_ = nullptr;
    QTabWidget *sideTabs_ = nullptr;
    QListWidget *chats_ = nullptr;
    QLineEdit *chatSearch_ = nullptr;
    QLineEdit *messageSearch_ = nullptr;
    QLineEdit *suggestionsFile_ = nullptr;
    QLineEdit *settingsSearch_ = nullptr;
    QWidget *suggestionsCanvas_ = nullptr;
    QVBoxLayout *suggestionsLayout_ = nullptr;
    QScrollArea *scroll_ = nullptr;
    QVBoxLayout *messagesLayout_ = nullptr;
    QWidget *messagesCanvas_ = nullptr;
    QPointer<ChatTextBrowser> pendingReply_;
    QPointer<QWidget> pendingRow_;
    QLabel *title_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QLabel *contextLabel_ = nullptr;
    QLabel *progressLabel_ = nullptr;
    QLabel *statisticsLabel_ = nullptr;
    QLabel *packageLabel_ = nullptr;
    QLabel *runtimeLabel_ = nullptr;
    QLabel *composerHint_ = nullptr;
    QProgressBar *progress_ = nullptr;
    QTextEdit *input_ = nullptr;
    QTextEdit *customInstructions_ = nullptr;
    QPushButton *sendButton_ = nullptr;
    QPushButton *stopButton_ = nullptr;
    QPushButton *pauseButton_ = nullptr;
    QPushButton *settingsButton_ = nullptr;
    QPushButton *sidebarButton_ = nullptr;
    QPushButton *latestButton_ = nullptr;
    QPushButton *cancelEditButton_ = nullptr;
    QComboBox *themeChoice_ = nullptr;
    QComboBox *answerStyle_ = nullptr;
    QComboBox *performance_ = nullptr;
    QComboBox *enterMode_ = nullptr;
    QComboBox *refreshRate_ = nullptr;
    QComboBox *groupFilter_ = nullptr;
    QComboBox *chatOrder_ = nullptr;
    QSpinBox *fontSize_ = nullptr;
    QSpinBox *maxTokens_ = nullptr;
    QSpinBox *contextTokens_ = nullptr;
    QSpinBox *topK_ = nullptr;
    QSpinBox *chatWidth_ = nullptr;
    QSpinBox *historyTurns_ = nullptr;
    QDoubleSpinBox *temperature_ = nullptr;
    QDoubleSpinBox *repeatPenalty_ = nullptr;
    QCheckBox *includeSources_ = nullptr;
    QCheckBox *chatMl_ = nullptr;
    QCheckBox *compact_ = nullptr;
    QCheckBox *timestamps_ = nullptr;
    QCheckBox *animations_ = nullptr;
    QCheckBox *autoScroll_ = nullptr;
    QCheckBox *prepareWhileTyping_ = nullptr;
    QCheckBox *saveHistory_ = nullptr;
    QCheckBox *showVisuals_ = nullptr;
    QTimer *renderTimer_ = nullptr;
    QTimer *prepareTimer_ = nullptr;
};
