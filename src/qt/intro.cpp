// Copyright (c) 2011-present The Bitcoin Core developers
// Copyright (c) 2025-present The Elektron Net developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <chainparams.h>
#include <qt/intro.h>
#include <qt/forms/ui_intro.h>
#include <util/chaintype.h>
#include <util/fs.h>

#include <qt/freespacechecker.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>

#include <common/args.h>
#include <interfaces/node.h>
#include <util/fs_helpers.h>
#include <validation.h>

#include <QFileDialog>
#include <QSettings>
#include <QMessageBox>

#include <cmath>

namespace {
//! Elektron Net always uses manual pruning; retention is fixed at 137 days.
int GetPruneDisplayGB()
{
    return ELEKTRON_MANDATORY_PRUNE_WINDOW_GB;
}

int64_t MeasureDataDirUsageGiB(const fs::path& datadir)
{
    static constexpr const char* STORAGE_DIRS[]{"blocks", "chainstate", "snapshots"};
    uint64_t total{0};
    for (const char* subdir : STORAGE_DIRS) {
        total += DirectorySize(datadir / subdir);
    }
    if (total == 0) return -1;
    return static_cast<int64_t>((total + GB_BYTES - 1) / GB_BYTES);
}
} // namespace

Intro::Intro(QWidget *parent, int64_t blockchain_size_gb, int64_t chain_state_size_gb) :
    QDialog(parent, GUIUtil::dialog_flags),
    ui(new Ui::Intro),
    m_blockchain_size_gb(blockchain_size_gb),
    m_chain_state_size_gb(chain_state_size_gb),
    m_prune_target_gb{GetPruneDisplayGB()}
{
    ui->setupUi(this);
    ui->welcomeLabel->setText(ui->welcomeLabel->text().arg(CLIENT_NAME));
    ui->storageLabel->setText(ui->storageLabel->text().arg(CLIENT_NAME));

    ui->lblExplanation1->setText(ui->lblExplanation1->text()
        .arg(CLIENT_NAME)
        .arg(m_blockchain_size_gb)
        .arg(2009)
        .arg(tr("Elektron Net"))
    );
    ui->lblExplanation2->setText(ui->lblExplanation2->text().arg(CLIENT_NAME));

    // Elektron Net: mandatory 137-day pruning — not user-configurable.
    ui->prune->setChecked(true);
    ui->prune->setEnabled(false);
    ui->pruneGB->setValue(m_prune_target_gb);
    ui->pruneGB->setEnabled(false);
    ui->pruneGB->setToolTip(tr("Shows current on-disk usage (blocks, chainstate, snapshots) when the data directory already exists; otherwise the planning estimate for the mandatory 137-day window (~197,280 blocks at 60 s spacing)."));
    ui->lblPruneSuffix->setToolTip(ui->pruneGB->toolTip());
    UpdatePruneLabels(ui->prune->isChecked());

    startThread();
}

Intro::~Intro()
{
    delete ui;
    /* Ensure thread is finished before it is deleted */
    thread->quit();
    thread->wait();
}

QString Intro::getDataDirectory()
{
    return ui->dataDirectory->text();
}

void Intro::setDataDirectory(const QString &dataDir)
{
    ui->dataDirectory->setText(dataDir);
    if(dataDir == GUIUtil::getDefaultDataDirectory())
    {
        ui->dataDirDefault->setChecked(true);
        ui->dataDirectory->setEnabled(false);
        ui->ellipsisButton->setEnabled(false);
    } else {
        ui->dataDirCustom->setChecked(true);
        ui->dataDirectory->setEnabled(true);
        ui->ellipsisButton->setEnabled(true);
    }
}

int64_t Intro::getPruneMiB() const
{
    // -prune=1: manual pruning mode (no disk-size cap; MANDATORY_PRUNE_DEPTH applies).
    return 1;
}

bool Intro::showIfNeeded(bool& did_show_intro, int64_t& prune_MiB)
{
    did_show_intro = false;

    QSettings settings;
    /* If data directory provided on command line, no need to look at settings
       or show a picking dialog */
    if(!gArgs.GetArg("-datadir", "").empty())
        return true;
    /* 1) Default data directory for operating system */
    QString dataDir = GUIUtil::getDefaultDataDirectory();
    /* 2) Allow QSettings to override default dir */
    dataDir = settings.value("strDataDir", dataDir).toString();

    if(!fs::exists(GUIUtil::QStringToPath(dataDir)) || gArgs.GetBoolArg("-choosedatadir", DEFAULT_CHOOSE_DATADIR) || settings.value("fReset", false).toBool() || gArgs.GetBoolArg("-resetguisettings", false))
    {
        /* Use selectParams here to guarantee Params() can be used by node interface */
        try {
            SelectParams(gArgs.GetChainType());
        } catch (const std::exception&) {
            return false;
        }

        /* If current default data directory does not exist, let the user choose one */
        Intro intro(nullptr, Params().AssumedBlockchainSize(), Params().AssumedChainStateSize());
        intro.setDataDirectory(dataDir);
        intro.setWindowIcon(QIcon(":icons/bitcoin"));
        did_show_intro = true;

        while(true)
        {
            if(!intro.exec())
            {
                /* Cancel clicked */
                return false;
            }
            dataDir = intro.getDataDirectory();
            try {
                if (TryCreateDirectories(GUIUtil::QStringToPath(dataDir))) {
                    // If a new data directory has been created, make wallets subdirectory too
                    TryCreateDirectories(GUIUtil::QStringToPath(dataDir) / "wallets");
                }
                break;
            } catch (const fs::filesystem_error&) {
                QMessageBox::critical(nullptr, CLIENT_NAME,
                    tr("Error: Specified data directory \"%1\" cannot be created.").arg(dataDir));
                /* fall through, back to choosing screen */
            }
        }

        // Additional preferences:
        prune_MiB = intro.getPruneMiB();

        settings.setValue("strDataDir", dataDir);
        settings.setValue("fReset", false);
    }
    /* Only override -datadir if different from the default, to make it possible to
     * override -datadir in the bitcoin.conf file in the default data directory
     * (to be consistent with bitcoind behavior)
     */
    if(dataDir != GUIUtil::getDefaultDataDirectory()) {
        gArgs.SoftSetArg("-datadir", fs::PathToString(GUIUtil::QStringToPath(dataDir))); // use OS locale for path setting
    }
    return true;
}

void Intro::setStatus(int status, const QString &message, quint64 bytesAvailable)
{
    switch(status)
    {
    case FreespaceChecker::ST_OK:
        ui->errorMessage->setText(message);
        ui->errorMessage->setStyleSheet("");
        break;
    case FreespaceChecker::ST_ERROR:
        ui->errorMessage->setText(tr("Error") + ": " + message);
        ui->errorMessage->setStyleSheet("QLabel { color: #800000 }");
        break;
    }
    /* Indicate number of bytes available */
    if(status == FreespaceChecker::ST_ERROR)
    {
        ui->freeSpace->setText("");
    } else {
        m_bytes_available = bytesAvailable;
        if (ui->prune->isEnabled() && m_prune_checkbox_is_default) {
            ui->prune->setChecked(m_bytes_available < (m_blockchain_size_gb + m_chain_state_size_gb + 10) * GB_BYTES);
        }
        UpdateFreeSpaceLabel();
    }
    /* Don't allow confirm in ERROR state */
    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(status != FreespaceChecker::ST_ERROR);

    if (status != FreespaceChecker::ST_ERROR) {
        UpdateMeasuredStorage(ui->dataDirectory->text());
    }
}

void Intro::UpdateFreeSpaceLabel()
{
    QString freeString = tr("%n GB of space available", "", m_bytes_available / GB_BYTES);
    if (m_bytes_available < m_required_space_gb * GB_BYTES) {
        freeString += " " + tr("(of %n GB needed)", "", m_required_space_gb);
        ui->freeSpace->setStyleSheet("QLabel { color: #800000 }");
    } else if (m_bytes_available / GB_BYTES - m_required_space_gb < 10) {
        freeString += " " + tr("(%n GB needed for full chain)", "", m_required_space_gb);
        ui->freeSpace->setStyleSheet("QLabel { color: #999900 }");
    } else {
        ui->freeSpace->setStyleSheet("");
    }
    ui->freeSpace->setText(freeString + ".");
}

void Intro::on_dataDirectory_textChanged(const QString &dataDirStr)
{
    /* Disable OK button until check result comes in */
    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);
    checkPath(dataDirStr);
}

void Intro::on_ellipsisButton_clicked()
{
    QString dir = QDir::toNativeSeparators(QFileDialog::getExistingDirectory(nullptr, tr("Choose data directory"), ui->dataDirectory->text()));
    if(!dir.isEmpty())
        ui->dataDirectory->setText(dir);
}

void Intro::on_dataDirDefault_clicked()
{
    setDataDirectory(GUIUtil::getDefaultDataDirectory());
}

void Intro::on_dataDirCustom_clicked()
{
    ui->dataDirectory->setEnabled(true);
    ui->ellipsisButton->setEnabled(true);
}

void Intro::startThread()
{
    thread = new QThread(this);
    FreespaceChecker *executor = new FreespaceChecker(this);
    executor->moveToThread(thread);

    connect(executor, &FreespaceChecker::reply, this, &Intro::setStatus);
    connect(this, &Intro::requestCheck, executor, &FreespaceChecker::check);
    /*  make sure executor object is deleted in its own thread */
    connect(thread, &QThread::finished, executor, &QObject::deleteLater);

    thread->start();
}

void Intro::checkPath(const QString &dataDir)
{
    mutex.lock();
    pathToCheck = dataDir;
    if(!signalled)
    {
        signalled = true;
        Q_EMIT requestCheck();
    }
    mutex.unlock();
}

QString Intro::getPathToCheck()
{
    QString retval;
    mutex.lock();
    retval = pathToCheck;
    signalled = false; /* new request can be queued now */
    mutex.unlock();
    return retval;
}

void Intro::UpdateMeasuredStorage(const QString& dataDir)
{
    const int64_t measured{MeasureDataDirUsageGiB(GUIUtil::QStringToPath(dataDir))};
    m_measured_storage_gb = measured;
    if (measured >= 0) {
        m_prune_target_gb = measured;
        ui->pruneGB->setValue(static_cast<int>(measured));
    } else {
        m_prune_target_gb = GetPruneDisplayGB();
        ui->pruneGB->setValue(m_prune_target_gb);
    }
    UpdatePruneLabels(ui->prune->isChecked());
}

void Intro::UpdatePruneLabels(bool prune_checked)
{
    Q_UNUSED(prune_checked);
    const int64_t planning_gb = std::max<int64_t>(GetPruneDisplayGB(), m_blockchain_size_gb) + m_chain_state_size_gb;
    m_required_space_gb = m_measured_storage_gb >= 0 ? m_measured_storage_gb : planning_gb;
    ui->lblExplanation3->setVisible(true);
    ui->pruneGB->setEnabled(false);
    if (m_measured_storage_gb >= 0) {
        ui->lblPruneSuffix->setText(tr("(current on-disk usage)"));
        ui->sizeWarningLabel->setText(
            tr("%1 will download and store a copy of the Elektron Net block chain.").arg(CLIENT_NAME) + " " +
            tr("This data directory currently uses %1 GB (blocks, chainstate, snapshots).").arg(m_measured_storage_gb) + " " +
            tr("The wallet will also be stored in this directory.")
        );
    } else {
        ui->lblPruneSuffix->setText(tr("(137 days / 197,280 blocks — mandatory, not configurable)"));
        ui->sizeWarningLabel->setText(
            tr("%1 will download and store a copy of the Elektron Net block chain.").arg(CLIENT_NAME) + " " +
            tr("Approximately %1 GB for the mandatory 137-day block window (grows with network usage), plus chainstate.").arg(planning_gb) + " " +
            tr("The wallet will also be stored in this directory.")
        );
    }
    this->adjustSize();
}