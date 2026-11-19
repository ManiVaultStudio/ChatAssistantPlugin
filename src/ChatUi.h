#pragma once
#include <QColor>
#include <QFrame>
#include <QIcon>
#include <QJsonObject>
#include <QTextBrowser>
#include <QToolButton>

struct ChatTheme {
    QColor background, surface, elevated, text, muted, border, user, accent, accentText;
    bool dark = false;
    static ChatTheme resolve(const QString &name, const QColor &accent, bool systemDark);
};
QString readableTutorialMarkdown(const QString &text);
QIcon chatIcon(const QString &name, const QColor &color);
void chatTip(QWidget *widget, const QString &title, const QString &description, const QString &shortcut = {});

class ChatTextBrowser : public QTextBrowser {
    Q_OBJECT
  public:
    explicit ChatTextBrowser(QWidget *parent = nullptr);
    void setChatMarkdown(const QString &text, const ChatTheme &theme, int points);

  protected:
    void resizeEvent(QResizeEvent *event) override;

  private:
    void fitDocument();
    QString renderedText_, renderedStyle_;
    int renderedPoints_ = -1;
};

class ChatGroup : public QFrame {
    Q_OBJECT
  public:
    ChatGroup(const QString &title, QWidget *parent = nullptr);
    QWidget *body() const {
        return body_;
    }
    void setExpanded(bool expanded, bool animate = false);
    bool isExpanded() const {
        return header_->isChecked();
    }
    QToolButton *header() const {
        return header_;
    }

  private:
    QToolButton *header_;
    QWidget *body_;
};

class ChatChart : public QWidget {
    Q_OBJECT
  public:
    ChatChart(const QJsonObject &data, const ChatTheme &theme, QWidget *parent = nullptr);
    static QJsonObject fromMarkdown(const QString &text);
    QString csv() const;

  protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;

  private:
    QJsonObject data_;
    ChatTheme theme_;
    QList<QPointF> points_;
    QStringList labels_;
    QList<QRectF> hitRects_;
};
