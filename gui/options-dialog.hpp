#pragma once

#include "ui_options-dialog.h"
#include "logic/shortcuts.h"
#include <QObject>
#include <QWidget>
#include <functional>

class OptionsDialog : public QWidget
{
    Q_OBJECT
signals:
    void closing();
public:
    OptionsDialog(std::function<void(bool)> pause_keybindings);
private:
    main_settings main;
    std::function<void(bool)> pause_keybindings;
    Ui::options_dialog ui;
    void closeEvent(QCloseEvent *) override { doCancel(); }
    static QString kopts_to_string(const key_opts& opts);
private slots:
    void doOK();
    void doCancel();
    void bind_key(key_opts &kopts, QLabel* label);
    void set_disable_translation_state(bool value);
};
