#include "ChatUi.h"
#include "ManiVaultControls.h"
#ifdef ASSISTANT_MANIVAULT_ACTIONS
#include <actions/WidgetAction.h>
#endif
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QTextDocument>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <QtTooltipKit.h>
#include <algorithm>
#include <cmath>

ChatTheme ChatTheme::resolve(const QString &name, const QColor &custom, bool systemDark) {
    ChatTheme t;
    t.dark = name == "Dark" || name == "Midnight" || name == "Nord" || name == "Graphite" ||
             name == "High contrast" || (name == "System" && systemDark);
    t.background = t.dark ? QColor("#111827") : QColor("#f5f7fb");
    t.surface = t.dark ? QColor("#192332") : QColor("#ffffff");
    t.elevated = t.dark ? QColor("#233044") : QColor("#edf1f8");
    t.text = t.dark ? QColor("#edf2fa") : QColor("#182235");
    t.muted = t.dark ? QColor("#a7b5c8") : QColor("#596a82");
    t.border = t.dark ? QColor("#344156") : QColor("#dce3ee");
    if (name == "Midnight") {
        t.background = QColor("#090e1c");
        t.surface = QColor("#101a2d");
        t.elevated = QColor("#1a2943");
    }
    if (name == "Nord") {
        t.background = QColor("#242b36");
        t.surface = QColor("#2e3745");
        t.elevated = QColor("#3b4759");
        t.border = QColor("#526078");
    }
    if (name == "Graphite") {
        t.background = QColor("#17181c");
        t.surface = QColor("#22242a");
        t.elevated = QColor("#30333b");
        t.border = QColor("#454953");
    }
    if (name == "Sepia") {
        t.background = QColor("#f3eee3");
        t.surface = QColor("#fffbf2");
        t.elevated = QColor("#eae3d4");
        t.text = QColor("#382f25");
        t.muted = QColor("#75614b");
        t.border = QColor("#d5c9b5");
    }
    if (name == "Rose") {
        t.background = QColor("#fbf4f7");
        t.surface = QColor("#fffafb");
        t.elevated = QColor("#f5e8ef");
        t.border = QColor("#e6d3df");
    }
    if (name == "High contrast") {
        t.background = Qt::black;
        t.surface = Qt::black;
        t.elevated = QColor("#191919");
        t.text = Qt::white;
        t.muted = QColor("#dddddd");
        t.border = Qt::white;
    }
    t.accent = custom.isValid() ? custom : QColor(t.dark ? "#9aa8ff" : "#4f63d8");
    if (name == "High contrast")
        t.accent = QColor("#ffe45c");
    const auto luminance = [](QColor c) {
        auto f = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
        return .2126 * f(c.redF()) + .7152 * f(c.greenF()) + .0722 * f(c.blueF());
    };
    const double l = luminance(t.accent);
    t.accentText = (l + .05) / .05 > 1.05 / (l + .05) ? QColor("#080c15") : Qt::white;
    t.user = t.dark ? t.elevated : QColor("#eaf0ff");
    return t;
}

QIcon chatIcon(const QString &name, const QColor &color) {
    QPixmap pix(40, 40);
    pix.setDevicePixelRatio(2);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    if (name == "plus") {
        p.drawLine(5, 10, 15, 10);
        p.drawLine(10, 5, 10, 15);
    } else if (name == "send") {
        p.drawLine(3, 10, 17, 3);
        p.drawLine(17, 3, 11, 17);
        p.drawLine(11, 17, 9, 11);
        p.drawLine(9, 11, 3, 10);
        p.drawLine(9, 11, 17, 3);
    } else if (name == "stop") {
        p.setBrush(color);
        p.drawRoundedRect(QRectF(5, 5, 10, 10), 2, 2);
    } else if (name == "pause") {
        p.drawLine(7, 5, 7, 15);
        p.drawLine(13, 5, 13, 15);
    } else if (name == "play") {
        p.drawPolygon(QPolygonF{QPointF(7, 4), QPointF(16, 10), QPointF(7, 16)});
    } else if (name == "search") {
        p.drawEllipse(QRectF(3, 3, 10, 10));
        p.drawLine(12, 12, 17, 17);
    } else if (name == "copy") {
        p.drawRoundedRect(QRectF(7, 6, 10, 11), 2, 2);
        p.drawLine(4, 13, 4, 3);
        p.drawLine(4, 3, 13, 3);
    } else if (name == "trash") {
        p.drawLine(4, 5, 16, 5);
        p.drawLine(8, 2, 12, 2);
        p.drawRoundedRect(QRectF(5, 5, 10, 12), 1, 1);
        p.drawLine(8, 8, 8, 14);
        p.drawLine(12, 8, 12, 14);
    } else if (name == "settings") {
        for (int y : {5, 10, 15})
            p.drawLine(3, y, 17, y);
        p.drawEllipse(QPointF(7, 5), 2, 2);
        p.drawEllipse(QPointF(13, 10), 2, 2);
        p.drawEllipse(QPointF(8, 15), 2, 2);
    } else if (name == "branch") {
        p.drawEllipse(QPointF(6, 4), 2, 2);
        p.drawEllipse(QPointF(6, 16), 2, 2);
        p.drawEllipse(QPointF(15, 5), 2, 2);
        p.drawLine(6, 6, 6, 14);
        p.drawLine(6, 11, 15, 7);
    } else if (name == "download") {
        p.drawLine(10, 3, 10, 13);
        p.drawLine(6, 9, 10, 13);
        p.drawLine(10, 13, 14, 9);
        p.drawLine(4, 15, 4, 17);
        p.drawLine(4, 17, 16, 17);
        p.drawLine(16, 17, 16, 15);
    } else if (name == "chat") {
        p.drawRoundedRect(QRectF(3, 3, 14, 11), 3, 3);
        p.drawLine(6, 14, 5, 18);
        p.drawLine(5, 18, 10, 14);
        p.drawLine(6, 7, 14, 7);
        p.drawLine(6, 10, 11, 10);
    } else if (name == "sidebar") {
        p.drawRoundedRect(QRectF(2, 3, 16, 14), 2, 2);
        p.drawLine(7, 3, 7, 17);
    } else if (name == "star") {
        QPolygonF poly;
        for (int i = 0; i < 10; ++i) {
            double a = i * 3.14159265 / 5 - 1.5707963, r = i % 2 ? 3.3 : 7;
            poly << QPointF(10 + r * std::cos(a), 10 + r * std::sin(a));
        }
        p.drawPolygon(poly);
    } else if (name == "retry") {
        p.drawArc(QRectF(4, 4, 12, 12), 35 * 16, 285 * 16);
        p.drawLine(16, 3, 16, 8);
        p.drawLine(12, 7, 16, 8);
    } else if (name == "close") {
        p.drawLine(5, 5, 15, 15);
        p.drawLine(15, 5, 5, 15);
    } else if (name == "down") {
        p.drawLine(5, 7, 10, 12);
        p.drawLine(10, 12, 15, 7);
    } else if (name == "file") {
        p.drawRoundedRect(QRectF(5, 2, 11, 16), 1, 1);
        p.drawLine(8, 7, 13, 7);
        p.drawLine(8, 10, 13, 10);
        p.drawLine(8, 13, 12, 13);
    } else if (name == "chart") {
        p.drawLine(3, 3, 3, 17);
        p.drawLine(3, 17, 18, 17);
        p.drawLine(6, 14, 6, 10);
        p.drawLine(10, 14, 10, 6);
        p.drawLine(14, 14, 14, 3);
    } else {
        p.drawEllipse(QRectF(3, 3, 14, 14));
        p.drawLine(10, 6, 10, 10);
        p.drawPoint(10, 14);
    }
    return QIcon(pix);
}

void chatTip(QWidget *w, const QString &title, const QString &text, const QString &shortcut) {
    qttooltipkit::Options o;
    o.widthPixels = 300;
    o.durationMs = 12000;
    o.shortcut = shortcut;
    o.accent = QColor("#7f91ed");
    qttooltipkit::set(*w, title, text, o);
#ifdef ASSISTANT_MANIVAULT_ACTIONS
    if (auto *action = qobject_cast<mv::gui::WidgetAction *>(AssistantControls::action(w)))
        qttooltipkit::set(*action, title, text, o);
#endif
}

ChatTextBrowser::ChatTextBrowser(QWidget *p) : QTextBrowser(p) {
    setFrameShape(QFrame::NoFrame);
    setOpenLinks(false);
    setOpenExternalLinks(false);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    document()->setDocumentMargin(0);
    connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this,
            [this] { fitDocument(); });
}
void ChatTextBrowser::fitDocument() {
    int h = std::max(24, int(std::ceil(document()->size().height())) + 3);
    if (height() != h)
        setFixedHeight(h);
}
void ChatTextBrowser::resizeEvent(QResizeEvent *e) {
    QTextBrowser::resizeEvent(e);
    fitDocument();
}
void ChatTextBrowser::setChatMarkdown(const QString &text, const ChatTheme &t, int points) {
    const auto style =
        QString("body,p,li,td,th {color:%1;} p {margin-top:3px;margin-bottom:8px;} h1,h2,h3 {color:%1;} a "
                "{color:%2;} pre {background:%3;white-space:pre-wrap;padding:10px;} code "
                "{font-family:monospace;} th,td {padding:6px;border:1px solid %4;} blockquote {color:%5;}")
            .arg(t.text.name(), t.accent.name(), t.elevated.name(), t.border.name(), t.muted.name());
    if (renderedText_ == text && renderedStyle_ == style && renderedPoints_ == points)
        return;
    if (renderedPoints_ != points)
        document()->setDefaultFont(QFont(font().family(), points));
    if (renderedStyle_ != style)
        document()->setDefaultStyleSheet(style);
    renderedText_ = text;
    renderedStyle_ = style;
    renderedPoints_ = points;
    document()->setMarkdown(text, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub) |
                                      QTextDocument::MarkdownNoHTML);
    fitDocument();
}

ChatGroup::ChatGroup(const QString &title, QWidget *p) : QFrame(p) {
    setObjectName("collapsibleGroup");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    header_ = new QToolButton(this);
    header_->setText(title);
    header_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    header_->setCheckable(true);
    header_->setArrowType(Qt::RightArrow);
    header_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    header_->setMinimumWidth(0);
    body_ = new QWidget(this);
    body_->hide();
    layout->addWidget(header_);
    layout->addWidget(body_);
    connect(header_, &QToolButton::toggled, this,
            [this](bool expanded) { setExpanded(expanded, property("animations").toBool()); });
}
void ChatGroup::setExpanded(bool expanded, bool animate) {
    QSignalBlocker block(header_);
    header_->setChecked(expanded);
    header_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    for (auto *animation : body_->findChildren<QPropertyAnimation *>())
        animation->stop();
    if (!animate) {
        body_->setMaximumHeight(QWIDGETSIZE_MAX);
        body_->setVisible(expanded);
        return;
    }
    if (expanded)
        body_->show();
    auto *animation = new QPropertyAnimation(body_, "maximumHeight", body_);
    animation->setDuration(130);
    animation->setStartValue(expanded ? 0 : body_->height());
    animation->setEndValue(expanded ? body_->sizeHint().height() : 0);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(animation, &QPropertyAnimation::finished, body_, [this, expanded] {
        body_->setVisible(expanded);
        body_->setMaximumHeight(QWIDGETSIZE_MAX);
    });
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

ChatChart::ChatChart(const QJsonObject &data, const ChatTheme &theme, QWidget *p)
    : QWidget(p), data_(data), theme_(theme) {
    setMinimumHeight(210);
    setMaximumHeight(280);
    setMouseTracking(true);
    const auto values = data["values"].toArray();
    const auto labels = data["labels"].toArray();
    for (int i = 0; i < values.size() && i < 128; ++i)
        if (values[i].isDouble() && std::isfinite(values[i].toDouble())) {
            points_ << QPointF(i, values[i].toDouble());
            labels_ << (i < labels.size() ? labels[i].toString() : QString::number(i + 1));
        }
    if (points_.isEmpty())
        for (const auto &value : data["points"].toArray()) {
            auto a = value.toArray();
            if (a.size() == 2 && a[0].isDouble() && a[1].isDouble() && std::isfinite(a[0].toDouble()) &&
                std::isfinite(a[1].toDouble()) && points_.size() < 512) {
                points_ << QPointF(a[0].toDouble(), a[1].toDouble());
                labels_ << QString::number(points_.size());
            }
        }
}
QJsonObject ChatChart::fromMarkdown(const QString &text) {
    auto match =
        QRegularExpression("```chart\\s*\\n([\\s\\S]*?)```", QRegularExpression::CaseInsensitiveOption)
            .match(text);
    if (!match.hasMatch() || match.captured(1).size() > 65536)
        return {};
    auto object = QJsonDocument::fromJson(match.captured(1).toUtf8()).object();
    if (!QStringList{"bar", "line", "scatter"}.contains(object["type"].toString()))
        return {};
    if (object["values"].toArray().isEmpty() && object["points"].toArray().isEmpty())
        return {};
    return object;
}
QString ChatChart::csv() const {
    QString out = "label,x,value\n";
    for (int i = 0; i < points_.size(); ++i) {
        QString label = labels_.value(i);
        label.replace('"', "\"\"");
        out += '"' + label + '"' + ',' + QString::number(points_[i].x()) + ',' +
               QString::number(points_[i].y()) + '\n';
    }
    return out;
}
void ChatChart::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), theme_.elevated);
    p.setPen(theme_.text);
    p.drawText(QRect(16, 8, width() - 32, 26), Qt::AlignLeft | Qt::AlignVCenter,
               data_["title"].toString("Chart"));
    if (points_.isEmpty())
        return;
    QRectF plot(48, 42, std::max(1, width() - 70), std::max(1, height() - 80));
    double lo = 0, hi = 0, minX = points_.first().x(), maxX = minX;
    for (auto v : points_) {
        lo = std::min(lo, v.y());
        hi = std::max(hi, v.y());
        minX = std::min(minX, v.x());
        maxX = std::max(maxX, v.x());
    }
    if (hi == lo)
        hi = lo + 1;
    if (maxX == minX)
        maxX = minX + 1;
    auto y = [&](double value) { return plot.bottom() - (value - lo) / (hi - lo) * plot.height(); };
    p.setPen(theme_.border);
    for (int i = 0; i < 4; ++i) {
        double value = lo + (hi - lo) * i / 3;
        p.drawLine(QPointF(plot.left(), y(value)), QPointF(plot.right(), y(value)));
        p.setPen(theme_.muted);
        p.drawText(QRectF(1, y(value) - 10, 42, 20), Qt::AlignRight | Qt::AlignVCenter,
                   QString::number(value, 'g', 3));
        p.setPen(theme_.border);
    }
    hitRects_.clear();
    QPainterPath path;
    const QString type = data_["type"].toString();
    for (int i = 0; i < points_.size(); ++i) {
        double x = type == "bar" ? plot.left() + (i + .5) * plot.width() / points_.size()
                                 : plot.left() + (points_[i].x() - minX) / (maxX - minX) * plot.width();
        QPointF point(x, y(points_[i].y()));
        p.setPen(QPen(theme_.accent, 2));
        p.setBrush(theme_.accent);
        if (type == "bar") {
            QRectF bar(x - plot.width() / points_.size() * .3, std::min(y(0), point.y()),
                       std::max(2.0, plot.width() / points_.size() * .6),
                       std::max(1.0, std::abs(y(0) - point.y())));
            p.drawRoundedRect(bar, 3, 3);
            hitRects_ << bar;
        } else {
            p.drawEllipse(point, 3, 3);
            hitRects_ << QRectF(point.x() - 7, point.y() - 7, 14, 14);
            if (i == 0)
                path.moveTo(point);
            else
                path.lineTo(point);
        }
        if (points_.size() <= 10) {
            p.setPen(theme_.muted);
            p.drawText(QRectF(x - 40, plot.bottom() + 7, 80, 22), Qt::AlignCenter,
                       fontMetrics().elidedText(labels_.value(i), Qt::ElideRight, 75));
        }
    }
    if (type == "line") {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(theme_.accent, 2));
        p.drawPath(path);
    }
}
void ChatChart::mouseMoveEvent(QMouseEvent *e) {
    for (int i = 0; i < hitRects_.size(); ++i)
        if (hitRects_[i].adjusted(-3, -3, 3, 3).contains(e->position())) {
            QToolTip::showText(e->globalPosition().toPoint(),
                               labels_.value(i) + ": " + QString::number(points_[i].y()), this);
            return;
        }
    QToolTip::hideText();
}
