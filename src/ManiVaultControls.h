#pragma once
#include <QtWidgets>
#include <functional>

namespace AssistantControls {
QWidget *container(QWidget *control);
QAction *action(QWidget *control);
QAction *command(QMenu *menu, const QString &title, const QIcon &icon = {});
QPushButton *trigger(QObject *owner, const QString &title, QWidget *parent, bool toggle = false,
                     bool iconOnly = false);
void setVisible(QWidget *control, bool visible);
void setEnabled(QWidget *control, bool enabled);
QComboBox *option(QObject *owner, const QString &title, const QStringList &values, QWidget *parent);
void setOptions(QComboBox *control, const QStringList &values, const QString &selected);
void select(QComboBox *control, const QString &value);
void select(QComboBox *control, int index);
QCheckBox *toggle(QObject *owner, const QString &title, QWidget *parent);
QSpinBox *integer(QObject *owner, const QString &title, int minimum, int maximum, QWidget *parent);
QDoubleSpinBox *decimal(QObject *owner, const QString &title, double minimum, double maximum,
                        QWidget *parent);
QLineEdit *line(QObject *owner, const QString &title, QWidget *parent);
QTextEdit *text(QObject *owner, const QString &title, QWidget *parent);
void onTrigger(QPushButton *control, QObject *owner, std::function<void()> callback);
void onToggle(QPushButton *control, QObject *owner, std::function<void(bool)> callback);
void onChanged(QWidget *control, QObject *owner, std::function<void()> callback);
void onOption(QComboBox *control, QObject *owner, std::function<void(int)> callback);

class VBoxLayout : public QVBoxLayout {
  public:
    using QVBoxLayout::QVBoxLayout;
    void addWidget(QWidget *w, int stretch = 0, Qt::Alignment alignment = {}) {
        QVBoxLayout::addWidget(container(w), stretch, alignment);
    }
};
class HBoxLayout : public QHBoxLayout {
  public:
    using QHBoxLayout::QHBoxLayout;
    void addWidget(QWidget *w, int stretch = 0, Qt::Alignment alignment = {}) {
        QHBoxLayout::addWidget(container(w), stretch, alignment);
    }
};
class FormLayout : public QFormLayout {
  public:
    using QFormLayout::addRow;
    using QFormLayout::QFormLayout;
    void addRow(QWidget *w) {
        QFormLayout::addRow(container(w));
    }
    void addRow(const QString &label, QWidget *w) {
        QFormLayout::addRow(label, container(w));
    }
};
} // namespace AssistantControls
