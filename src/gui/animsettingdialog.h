#ifndef ANIMSETTINGDIALOG_H
#define ANIMSETTINGDIALOG_H

#include <QCheckBox>
#include <QDialog>
#include <QFrame>
#include <QLabel>
#include <QProcess>
#include <QSpinBox>
#include <QTimer>
#include "kbanim.h"
#include <QComboBox>

namespace Ui {
class AnimSettingDialog;
}

class AnimSettingDialog : public QDialog
{
    Q_OBJECT

public:
    // Create with the dialog parent, and the animation to edit. Use exec() to present.
    explicit AnimSettingDialog(QWidget* parent, KbAnim* anim);
    ~AnimSettingDialog();

    // Name entered for the animation
    QString name() const;

private:
    Ui::AnimSettingDialog* ui;
    QCheckBox*  stopCheck, *kpStopCheck;
    bool        hasRepeat;

    KbAnim* _anim;
    double  lastDuration;
    QMap<QString, QWidget*> settingWidgets;

    QMap<QString, QSpinBox*>    angleSpinners;

    // Live display value for one "list" param (e.g. a sensor reading), fetched by repeatedly
    // running the script's own "--ckb-query" one-shot mode -- see hasQuery() in animscript.h.
    // Only the first query-capable list param found gets this treatment; a second one (none
    // exist today) would simply not get a live label.
    QString  queryParamName;
    QLabel*  queryLabel = nullptr;
    QComboBox* queryCombo = nullptr;
    QProcess* queryProcess = nullptr;
    QTimer*  queryTimer = nullptr;
    // Bumped every time pollQueryValue() actually starts a query -- lets each query's watchdog
    // timer (below) tell whether it's still watching the run it was scheduled for, or a stale
    // one from an earlier poll (queryProcess is reused, so state() alone can't tell them apart).
    int      queryGeneration = 0;
    void pollQueryValue();

    QFrame* hLine();

private slots:
    void newDuration(double duration);
    void updateStops();
    void angleDialChanged(const QString& name);
    void angleSpinnerChanged(const QString& name);
    void updateParam(const QString& name);
    void on_delayBox_valueChanged(double arg1);
    void on_repeatBox_valueChanged(double arg1);
    void on_kpDelayBox_valueChanged(double arg1);
    void on_kpRepeatBox_valueChanged(double arg1);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    void on_kpReleaseBox_checkStateChanged(Qt::CheckState arg1);
#else // QT_VERSION < 6.7.0
    void on_kpReleaseBox_stateChanged(int arg1);
#endif
};

#endif // ANIMSETTINGDIALOG_H
