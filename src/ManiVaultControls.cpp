#include "ManiVaultControls.h"
#ifdef ASSISTANT_MANIVAULT_ACTIONS
#include <actions/DecimalAction.h>
#include <actions/GroupAction.h>
#include <actions/IntegralAction.h>
#include <actions/OptionAction.h>
#include <actions/StringAction.h>
#include <actions/ToggleAction.h>
#include <actions/TriggerAction.h>
#endif

namespace AssistantControls {
QAction *command(QMenu *menu, const QString &title, const QIcon &icon) {
#ifdef ASSISTANT_MANIVAULT_ACTIONS
    auto *a = new mv::gui::TriggerAction(menu, title);
    a->setIcon(icon);
    menu->addAction(a);
    return a;
#else
    return menu->addAction(icon, title);
#endif
}
QWidget *container(QWidget *w) {
    auto *root = qobject_cast<QWidget *>(w->property("assistantContainer").value<QObject *>());
    return root ? root : w;
}
QAction *action(QWidget *w) {
    return qobject_cast<QAction *>(w->property("assistantAction").value<QObject *>());
}
void setVisible(QWidget *w, bool visible) {
    if (auto *a = action(w))
        a->setVisible(visible);
    container(w)->setVisible(visible);
}
void setEnabled(QWidget *w, bool enabled) {
    if (auto *a = action(w))
        a->setEnabled(enabled);
    else
        w->setEnabled(enabled);
}
#ifdef ASSISTANT_MANIVAULT_ACTIONS
template <typename Control, typename Action> Control *widget(Action *action, QWidget *parent, int flags) {
    if (auto *group = qobject_cast<mv::gui::GroupAction *>(action->parent()))
        group->addAction(action);
    QWidget *root = action->createWidget(parent, flags);
    auto *control = qobject_cast<Control *>(root);
    if (!control)
        control = root->findChild<Control *>();
    QObject::connect(root, &QObject::destroyed, action, &QObject::deleteLater);
    Q_ASSERT(control);
    control->setProperty("assistantContainer", QVariant::fromValue<QObject *>(root));
    control->setProperty("assistantAction", QVariant::fromValue<QObject *>(action));
    return control;
}
#endif
QPushButton *trigger(QObject *owner, const QString &title, QWidget *parent, bool checkable, bool iconOnly) {
#ifdef ASSISTANT_MANIVAULT_ACTIONS
    if (checkable) {
        auto *a = new mv::gui::ToggleAction(owner, title);
        return widget<QPushButton>(a, parent,
                                   iconOnly ? mv::gui::ToggleAction::PushButtonIcon
                                            : mv::gui::ToggleAction::PushButtonIconText);
    }
    auto *a = new mv::gui::TriggerAction(owner, title);
    return widget<QPushButton>(a, parent,
                               iconOnly ? mv::gui::TriggerAction::Icon : mv::gui::TriggerAction::IconText);
#else
    Q_UNUSED(owner);
    auto *b = new QPushButton(iconOnly ? QString() : title, parent);
    b->setCheckable(checkable);
    return b;
#endif
}
QComboBox *option(QObject *owner, const QString &title, const QStringList &values, QWidget *parent) {
#ifdef ASSISTANT_MANIVAULT_ACTIONS
    auto *a = new mv::gui::OptionAction(owner, title, values, values.value(0));
    auto *control = widget<QComboBox>(a, parent, mv::gui::OptionAction::ComboBox);
    control->setMinimumWidth(0);
    return control;
#else
    Q_UNUSED(owner);
    Q_UNUSED(title);
    auto *w = new QComboBox(parent);
    w->addItems(values);
    return w;
#endif
}
void select(QComboBox *control, const QString &value) {
    const auto selected = control->findText(value) >= 0 ? value : control->itemText(0);
#ifdef ASSISTANT_MANIVAULT_ACTIONS
    if (auto *model = qobject_cast<mv::gui::OptionAction *>(action(control))) {
        model->setCurrentText(selected);
        return;
    }
#endif
    control->setCurrentText(selected);
}
void select(QComboBox *control, int index) {
    select(control, control->itemText(index));
}
void setOptions(QComboBox *control, const QStringList &values, const QString &selected) {
    QStringList existing;
    for (int i = 0; i < control->count(); ++i)
        existing << control->itemText(i);
    if (existing != values) {
#ifdef ASSISTANT_MANIVAULT_ACTIONS
        if (auto *model = qobject_cast<mv::gui::OptionAction *>(action(control)))
            model->setOptions(values);
        else
#endif
        {
            control->clear();
            control->addItems(values);
        }
    }
    select(control, values.contains(selected) ? selected : values.value(0));
}
QCheckBox *toggle(QObject *owner, const QString &title, QWidget *parent) {
#ifdef ASSISTANT_MANIVAULT_ACTIONS
    return widget<QCheckBox>(new mv::gui::ToggleAction(owner, title), parent,
                             mv::gui::ToggleAction::CheckBox);
#else
    Q_UNUSED(owner);
    return new QCheckBox(title, parent);
#endif
}
QSpinBox *integer(QObject *owner, const QString &title, int minimum, int maximum, QWidget *parent) {
#ifdef ASSISTANT_MANIVAULT_ACTIONS
    return widget<QSpinBox>(new mv::gui::IntegralAction(owner, title, minimum, maximum, minimum), parent,
                            mv::gui::IntegralAction::SpinBox);
#else
    Q_UNUSED(owner);
    Q_UNUSED(title);
    auto *w = new QSpinBox(parent);
    w->setRange(minimum, maximum);
    return w;
#endif
}
QDoubleSpinBox *decimal(QObject *owner, const QString &title, double minimum, double maximum,
                        QWidget *parent) {
#ifdef ASSISTANT_MANIVAULT_ACTIONS
    return widget<QDoubleSpinBox>(
        new mv::gui::DecimalAction(owner, title, float(minimum), float(maximum), float(minimum), 2), parent,
        mv::gui::DecimalAction::SpinBox);
#else
    Q_UNUSED(owner);
    Q_UNUSED(title);
    auto *w = new QDoubleSpinBox(parent);
    w->setRange(minimum, maximum);
    return w;
#endif
}
QLineEdit *line(QObject *owner, const QString &title, QWidget *parent) {
#ifdef ASSISTANT_MANIVAULT_ACTIONS
    return widget<QLineEdit>(new mv::gui::StringAction(owner, title), parent,
                             mv::gui::StringAction::LineEdit);
#else
    Q_UNUSED(owner);
    Q_UNUSED(title);
    return new QLineEdit(parent);
#endif
}
QTextEdit *text(QObject *owner, const QString &title, QWidget *parent) {
#ifdef ASSISTANT_MANIVAULT_ACTIONS
    return widget<QTextEdit>(new mv::gui::StringAction(owner, title), parent,
                             mv::gui::StringAction::TextEdit);
#else
    Q_UNUSED(owner);
    Q_UNUSED(title);
    return new QTextEdit(parent);
#endif
}
void onTrigger(QPushButton *w, QObject *owner, std::function<void()> callback) {
    if (auto *a = action(w))
        QObject::connect(a, &QAction::triggered, owner, [callback] { callback(); });
    else
        QObject::connect(w, &QPushButton::clicked, owner, [callback] { callback(); });
}
void onToggle(QPushButton *w, QObject *owner, std::function<void(bool)> callback) {
    if (auto *a = action(w))
        QObject::connect(a, &QAction::toggled, owner, [callback](bool value) { callback(value); });
    else
        QObject::connect(w, &QPushButton::toggled, owner, callback);
}
void onOption(QComboBox *w, QObject *owner, std::function<void(int)> callback) {
#ifdef ASSISTANT_MANIVAULT_ACTIONS
    if (auto *a = qobject_cast<mv::gui::OptionAction *>(action(w))) {
        QObject::connect(a, &mv::gui::OptionAction::currentIndexChanged, owner,
                         [callback](const int &value) { callback(value); });
        return;
    }
#endif
    QObject::connect(w, &QComboBox::currentIndexChanged, owner, callback);
}
void onChanged(QWidget *w, QObject *owner, std::function<void()> callback) {
#ifdef ASSISTANT_MANIVAULT_ACTIONS
    if (auto *a = action(w)) {
        if (auto *v = qobject_cast<mv::gui::OptionAction *>(a))
            QObject::connect(v, &mv::gui::OptionAction::currentIndexChanged, owner,
                             [callback] { callback(); });
        else if (auto *v = qobject_cast<mv::gui::ToggleAction *>(a))
            QObject::connect(v, &QAction::toggled, owner, [callback] { callback(); });
        else if (auto *v = qobject_cast<mv::gui::IntegralAction *>(a))
            QObject::connect(v, &mv::gui::IntegralAction::valueChanged, owner, [callback] { callback(); });
        else if (auto *v = qobject_cast<mv::gui::DecimalAction *>(a))
            QObject::connect(v, &mv::gui::DecimalAction::valueChanged, owner, [callback] { callback(); });
        else if (auto *v = qobject_cast<mv::gui::StringAction *>(a))
            QObject::connect(v, &mv::gui::StringAction::stringChanged, owner, [callback] { callback(); });
        return;
    }
#endif
    if (auto *c = qobject_cast<QComboBox *>(w))
        QObject::connect(c, &QComboBox::currentIndexChanged, owner, [callback] { callback(); });
    else if (auto *c = qobject_cast<QCheckBox *>(w))
        QObject::connect(c, &QCheckBox::toggled, owner, [callback] { callback(); });
    else if (auto *c = qobject_cast<QSpinBox *>(w))
        QObject::connect(c, &QSpinBox::valueChanged, owner, [callback] { callback(); });
    else if (auto *c = qobject_cast<QDoubleSpinBox *>(w))
        QObject::connect(c, &QDoubleSpinBox::valueChanged, owner, [callback] { callback(); });
    else if (auto *c = qobject_cast<QLineEdit *>(w))
        QObject::connect(c, &QLineEdit::textChanged, owner, [callback] { callback(); });
    else if (auto *c = qobject_cast<QTextEdit *>(w))
        QObject::connect(c, &QTextEdit::textChanged, owner, [callback] { callback(); });
}
} // namespace AssistantControls
