#include "anonsendconfig.h"
#include "ui_anonsendconfig.h"

#include "bitcoinunits.h"
#include "guiconstants.h"
#include "optionsmodel.h"
#include "anonsend-client.h"
#include "walletmodel.h"

#include <QMessageBox>
#include <QPushButton>
#include <QKeyEvent>
#include <QSettings>

AnonsendConfig::AnonsendConfig(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::AnonsendConfig),
    model(0)
{
    ui->setupUi(this);

    connect(ui->buttonBasic, SIGNAL(clicked()), this, SLOT(clickBasic()));
    connect(ui->buttonHigh, SIGNAL(clicked()), this, SLOT(clickHigh()));
    connect(ui->buttonMax, SIGNAL(clicked()), this, SLOT(clickMax()));
}

AnonsendConfig::~AnonsendConfig()
{
    delete ui;
}

void AnonsendConfig::setModel(WalletModel *model)
{
    this->model = model;
}

void AnonsendConfig::clickBasic()
{
    configure(true, 1000, 2);

    QString strAmount(BitcoinUnits::formatWithUnit(
        model->getOptionsModel()->getDisplayUnit(), 1000 * COIN));
    QMessageBox::information(this, tr("AnonSend Configuration"),
        tr(
            "AnonSend was successfully set to basic (%1 and 2 rounds). You can change this at any time by opening MarteX's configuration screen."
        ).arg(strAmount)
    );

    close();
}

void AnonsendConfig::clickHigh()
{
    configure(true, 1000, 8);

    QString strAmount(BitcoinUnits::formatWithUnit(
        model->getOptionsModel()->getDisplayUnit(), 1000 * COIN));
    QMessageBox::information(this, tr("AnonSend Configuration"),
        tr(
            "AnonSend was successfully set to high (%1 and 8 rounds). You can change this at any time by opening MarteX's configuration screen."
        ).arg(strAmount)
    );

    close();
}

void AnonsendConfig::clickMax()
{
    configure(true, 1000, 16);

    QString strAmount(BitcoinUnits::formatWithUnit(
        model->getOptionsModel()->getDisplayUnit(), 1000 * COIN));
    QMessageBox::information(this, tr("AnonSend Configuration"),
        tr(
            "AnonSend was successfully set to maximum (%1 and 16 rounds). You can change this at any time by opening MarteX's configuration screen."
        ).arg(strAmount)
    );

    close();
}

void AnonsendConfig::configure(bool enabled, int coins, int rounds) {

    QSettings settings;

    settings.setValue("nAnonSendRounds", rounds);
    settings.setValue("nAnonSendAmount", coins);

    anonSendClient.nAnonSendRounds = rounds;
    anonSendClient.nAnonSendAmount = coins;
}
