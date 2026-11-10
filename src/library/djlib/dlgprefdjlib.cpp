#include "library/djlib/dlgprefdjlib.h"

#include <QDir>
#include <QFileDialog>
#include <QLocale>

#include "library/djlib/djlibservice.h"
#include "library/djlib/ui_dlgprefdjlibdlg.h"
#include "moc_dlgprefdjlib.cpp"
#include "proto/keys.pb.h" // needed by keydetectionsettings.h
#include "preferences/keydetectionsettings.h"
#include "preferences/replaygainsettings.h"

DlgPrefDjlib::DlgPrefDjlib(QWidget* pParent, UserSettingsPointer pConfig)
        : DlgPreferencePage(pParent),
          m_pUi(std::make_unique<Ui::DlgPrefDjlibDlg>()),
          m_pConfig(pConfig) {
    m_pUi->setupUi(this);
    connect(m_pUi->pushButtonBrowse, &QPushButton::clicked, this, &DlgPrefDjlib::slotBrowse);
    if (DjlibService* pService = DjlibService::instance()) {
        connect(pService,
                &DjlibService::statusChanged,
                this,
                &DlgPrefDjlib::slotUpdateStatus);
    }
    slotUpdate();
}

DlgPrefDjlib::~DlgPrefDjlib() = default;

void DlgPrefDjlib::slotUpdate() {
    m_pUi->lineEditLibraryPath->setText(
            m_pConfig->getValueString(DjlibService::kLibraryPathKey));
    slotUpdateStatus();
    updateWarning();
}

void DlgPrefDjlib::slotShow() {
    slotUpdateStatus();
    updateWarning();
}

void DlgPrefDjlib::slotApply() {
    const QString path = QDir::fromNativeSeparators(m_pUi->lineEditLibraryPath->text().trimmed());
    if (DjlibService* pService = DjlibService::instance()) {
        pService->setLibraryPath(path); // also stores the setting
    } else {
        m_pConfig->setValue(DjlibService::kLibraryPathKey, path);
    }
    slotUpdateStatus();
}

void DlgPrefDjlib::slotResetToDefaults() {
    m_pUi->lineEditLibraryPath->clear();
}

void DlgPrefDjlib::slotBrowse() {
    const QString current = m_pUi->lineEditLibraryPath->text().trimmed();
    const QString dir = QFileDialog::getExistingDirectory(this,
            tr("Choose the djlib library folder"),
            current.isEmpty() ? QDir::homePath() : current,
            QFileDialog::ShowDirsOnly);
    if (!dir.isEmpty()) {
        m_pUi->lineEditLibraryPath->setText(QDir::toNativeSeparators(dir));
    }
}

void DlgPrefDjlib::slotUpdateStatus() {
    const DjlibService* pService = DjlibService::instance();
    if (!pService || pService->libraryPath().isEmpty()) {
        m_pUi->labelWatched->setText(tr("Off (no folder chosen)"));
        m_pUi->labelLastImport->setText(QStringLiteral("-"));
        m_pUi->labelPending->setText(QStringLiteral("-"));
        return;
    }
    const auto status = pService->status();
    const QString tracks = QDir::toNativeSeparators(
            QDir(status.libraryPath).filePath(QStringLiteral("tracks")));
    m_pUi->labelWatched->setText(status.watching
                    ? tracks
                    : tr("%1 (folder not found yet)").arg(tracks));
    if (status.lastImport.isValid()) {
        m_pUi->labelLastImport->setText(tr("%1, %2").arg(
                QLocale().toString(status.lastImport, QLocale::ShortFormat),
                status.lastImportFile));
    } else {
        m_pUi->labelLastImport->setText(tr("None since Mixxx started"));
    }
    m_pUi->labelPending->setText(
            tr("%1 waiting for their audio file, %2 waiting for a deck to "
               "unload, %3 queued")
                    .arg(status.waitingForPackage)
                    .arg(status.deferred)
                    .arg(status.queued));
}

void DlgPrefDjlib::updateWarning() {
    QStringList settings;
    if (KeyDetectionSettings(m_pConfig).getReanalyzeWhenSettingsChange()) {
        settings << tr("Key Detection: \"Re-analyze keys when settings change or "
                       "3rd-party keys are present\"");
    }
    if (ReplayGainSettings(m_pConfig).getReplayGainReanalyze()) {
        settings << tr("Normalization: \"Re-analyze and override an existing value\"");
    }
    m_pUi->labelWarning->setVisible(!settings.isEmpty());
    if (!settings.isEmpty()) {
        m_pUi->labelWarning->setText(kWarningIconHtmlString +
                tr("Mixxx may replace keys or ReplayGain imported from djlib. "
                   "Consider turning off: %1.")
                        .arg(settings.join(QStringLiteral("; "))));
    }
}
