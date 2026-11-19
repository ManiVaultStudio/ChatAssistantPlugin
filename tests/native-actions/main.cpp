#include <QtWidgets>
#include <actions/OptionAction.h>
#include <actions/IntegralAction.h>
#include <actions/DecimalAction.h>
#include <actions/StringAction.h>
#include "ManiVaultControls.h"

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QObject owner;
    QWidget surface;
    auto *control = AssistantControls::option(&owner, "Test options", {"First", "Second", "Third"}, &surface);
    auto *model = qobject_cast<mv::gui::OptionAction *>(AssistantControls::action(control));
    if (!model || control->minimumWidth() > 0) return 1;
    int changed = 0;
    AssistantControls::onChanged(control, &surface, [&] { ++changed; });
    AssistantControls::select(control, "Third");
    if (model->getCurrentText() != "Third" || control->currentText() != "Third" || changed != 1) return 2;
    AssistantControls::setOptions(control, {"Alpha", "Beta"}, "Beta");
    if (model->getCurrentText() != "Beta" || control->currentText() != "Beta") return 3;
    AssistantControls::select(control, 0);
    if (model->getCurrentIndex() != 0 || control->currentIndex() != 0) return 4;
    model->setCurrentIndex(1);
    if (control->currentText() != "Beta") return 5;
    auto *command = AssistantControls::trigger(&owner, "Native command", &surface);
    int triggered = 0;
    AssistantControls::onTrigger(command, &surface, [&] { ++triggered; });
    command->click();
    if (triggered != 1) return 6;
    auto *number = AssistantControls::integer(&owner, "Integer", 0, 100, &surface);
    number->setValue(42);
    if (qobject_cast<mv::gui::IntegralAction *>(AssistantControls::action(number))->getValue() != 42) return 7;
    auto *decimal = AssistantControls::decimal(&owner, "Decimal", 0, 1, &surface);
    decimal->setValue(.35);
    if (std::abs(qobject_cast<mv::gui::DecimalAction *>(AssistantControls::action(decimal))->getValue() - .35) > .01) return 8;
    auto *editor = AssistantControls::text(&owner, "Text", &surface);
    editor->setAcceptRichText(false);
    const QString markdown = "A paragraph\n\n<tag>literal</tag> and **Markdown**";
    editor->setPlainText(markdown);
    if (qobject_cast<mv::gui::StringAction *>(AssistantControls::action(editor))->getString() != markdown || editor->toPlainText() != markdown) return 9;
    QTextStream(stdout) << "Native ManiVault OptionAction/TriggerAction widgets and model synchronization passed\n";
    return 0;
}
