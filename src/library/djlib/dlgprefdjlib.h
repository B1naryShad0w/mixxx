#pragma once

#include <memory>

#include "preferences/dialog/dlgpreferencepage.h"
#include "preferences/usersettings.h"

namespace Ui {
class DlgPrefDjlibDlg;
} // namespace Ui

/// Preferences page "djlib": the djlib library folder (`[djlib] LibraryPath`,
/// empty = off), the watcher status, and a warning when Mixxx is set to
/// re-analyze keys or ReplayGain (which could replace djlib's values).
class DlgPrefDjlib : public DlgPreferencePage {
    Q_OBJECT
  public:
    DlgPrefDjlib(QWidget* pParent, UserSettingsPointer pConfig);
    ~DlgPrefDjlib() override;

  public slots:
    void slotUpdate() override;
    void slotApply() override;
    void slotResetToDefaults() override;
    void slotShow() override;

  private slots:
    void slotBrowse();
    void slotUpdateStatus();

  private:
    void updateWarning();

    const std::unique_ptr<Ui::DlgPrefDjlibDlg> m_pUi;
    const UserSettingsPointer m_pConfig;
};
