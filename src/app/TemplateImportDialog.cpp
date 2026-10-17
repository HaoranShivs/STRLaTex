#include "app/TemplateImportDialog.h"
#include "app/ProjectController.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

namespace pf::gui {
TemplateImportDialog::TemplateImportDialog(ProjectController* controller,
                                           QString archive, QWidget* parent)
    : QDialog(parent), controller_(controller), archive_(std::move(archive)) {
    setObjectName("templateImportDialog");
    setWindowTitle("Import Template");
    resize(680, 480);
    auto* layout = new QVBoxLayout(this);
    auto* source = new QLabel(QFileInfo(archive_).fileName(), this);
    source->setTextFormat(Qt::PlainText);
    layout->addWidget(source);
    status_ = new QLabel("Inspecting template archive…", this);
    status_->setObjectName("templateImportStatus");
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    layout->addWidget(status_);
    details_ = new QPlainTextEdit(this);
    details_->setReadOnly(true);
    details_->setMaximumBlockCount(2000);
    details_->setObjectName("templateImportDetails");
    layout->addWidget(details_, 1);
    layout->addWidget(new QLabel("Template to use in this project:", this));
    profiles_ = new QComboBox(this);
    profiles_->setObjectName("templateImportProfiles");
    profiles_->setEnabled(false);
    layout->addWidget(profiles_);
    auto* note = new QLabel(
        "References use numeric citations in first-use order. You can switch "
        "between imported profiles in the top template selector.",
        this);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    import_ =
        buttons->addButton("Import and use", QDialogButtonBox::AcceptRole);
    import_->setObjectName("importTemplateButton");
    import_->setEnabled(false);
    connect(import_, &QPushButton::clicked, this,
            &TemplateImportDialog::Import);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    QTimer::singleShot(0, this, &TemplateImportDialog::Start);
}
TemplateImportDialog::~TemplateImportDialog() {
    cancelled_.store(true);
    if (worker_)
        worker_->wait();
}
void TemplateImportDialog::Start() {
    const auto runtime = controller_->session().texlive_root();
    const auto archive = archive_.toStdString();
    const auto workspace = workspace_.path().toStdString();
    if (!workspace_.isValid()) {
        status_->setText("Cannot create a template verification workspace");
        return;
    }
    worker_ = QThread::create([this, runtime, archive, workspace] {
        try {
            result_ = VerifyTemplateArchive(
                archive, runtime, workspace, &cancelled_,
                [this](const std::string& message) {
                    QMetaObject::invokeMethod(
                        this,
                        [this, text = QString::fromStdString(message)] {
                            status_->setText(text);
                            details_->appendPlainText(text);
                        },
                        Qt::QueuedConnection);
                });
        } catch (const std::exception& error) {
            result_ = Result<VerifiedTemplatePackage>{Unexpected(error.what())};
        }
    });
    worker_->setParent(this);
    connect(worker_, &QThread::finished, this, &TemplateImportDialog::Complete);
    worker_->start();
}
void TemplateImportDialog::Complete() {
    worker_->wait();
    if (!result_ || !*result_) {
        status_->setText("Template verification failed");
        details_->appendPlainText(result_
                                      ? QString::fromStdString(result_->error())
                                      : "No verification result");
        return;
    }
    const auto& verified = result_->value();
    for (const auto& def : verified.package.profiles)
        profiles_->addItem(QString::fromStdString(def.name),
                           QString::fromStdString(def.id));
    profiles_->setEnabled(true);
    import_->setEnabled(true);
    status_->setText(QString("Verified %1 template profiles")
                         .arg(verified.package.profiles.size()));
    details_->appendPlainText(
        "All profiles compiled successfully using their imported class files.");
}
void TemplateImportDialog::Import() {
    if (!result_ || !*result_)
        return;
    auto installed = controller_->InstallTemplate(
        result_->value(), profiles_->currentData().toString());
    if (!installed) {
        status_->setText("Cannot install template");
        details_->appendPlainText(QString::fromStdString(installed.error()));
        return;
    }
    accept();
}
} // namespace pf::gui
