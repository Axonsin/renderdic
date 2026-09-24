/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2017-2026 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 ******************************************************************************/

#include "CrashDialog.h"
#include <QApplication>
#include <QDateTime>
#include <QDesktopWidget>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QScreen>
#include <QStandardPaths>
#include <QString>
#include "Code/QRDUtils.h"
#include "ui_CrashDialog.h"

const qint64 MaxUploadSize = 2250LL * 1024LL * 1024LL;

CrashDialog::CrashDialog(PersistentConfig &cfg, QVariantMap crashReportJSON, QWidget *parent)
    : QDialog(parent), ui(new Ui::CrashDialog), m_Config(cfg)
{
  ui->setupUi(this);

  m_ReportPath = crashReportJSON[lit("report")].toString();

  const bool replayCrash = crashReportJSON[lit("replaycrash")].toUInt() != 0;
  const bool manualReport =
      crashReportJSON.contains(lit("manual")) && crashReportJSON[lit("manual")].toUInt() != 0;

  setStage(ReportStage::FillingDetails);

  if(!QFileInfo::exists(m_ReportPath))
  {
    // the zip was never created (or was cleaned up before we could read it) so there is nothing
    // to save - don't let the user proceed to a Save As that can only fail.
    ui->reportText->setTextFormat(Qt::RichText);
    ui->reportText->setText(
        tr("<p>Failed to create the crash report zip on disk, so there is nothing to save."
           "The renderdic log may still be available in the temporary folder.</p>"));
    ui->send->setEnabled(false);

    ui->captureLabel->hide();
    ui->captureFilename->hide();
    ui->capturePreviewFrame->hide();

    setWindowFlags((windowFlags() | Qt::MSWindowsFixedSizeDialogHint) &
                   ~Qt::WindowContextHelpButtonHint);
    adjustSize();
    return;
  }

  m_CaptureFilename = m_Config.CrashReport_LastOpenedCapture;

  QFileInfo capInfo(m_CaptureFilename);

  bool hasEmbeddedFiles = false;
  if(replayCrash && capInfo.exists())
  {
    // if we have a previous capture, fill out the capture group
    ui->captureFilename->setTextFormat(Qt::RichText);
    ui->captureFilename->setText(lit("<a href=\"file://%1\">%2</a>")
                                     .arg(QUrl::fromLocalFile(capInfo.absoluteFilePath()).toString())
                                     .arg(capInfo.fileName()));

    // hide the preview until we have a successful thumbnail
    ui->capturePreviewFrame->hide();

    ICaptureFile *cap = RENDERDOC_OpenCaptureFile();

    ResultDetails result = cap->OpenFile(capInfo.absoluteFilePath(), "", NULL);

    if(result.OK())
    {
      Thumbnail thumb = cap->GetThumbnail(FileType::Raw, 320);
      QImage i = QImage(thumb.data.data(), (int)thumb.width, (int)thumb.height, QImage::Format_RGB888)
                     .copy(0, 0, (int)thumb.width, (int)thumb.height);
      if(!i.isNull())
      {
        ui->capturePreview->setPixmap(QPixmap::fromImage(i));
        ui->capturePreview->setPreserveAspectRatio(true);
        ui->capturePreviewFrame->show();
      }
      hasEmbeddedFiles = cap->HasEmbeddedDependencies();
    }

    cap->Shutdown();
  }
  else
  {
    m_CaptureFilename = QString();

    // otherwise hide it entirely - this is probably a crash in the injected application or
    // something along those lines where a capture isn't directly associated.
    ui->captureLabel->hide();
    ui->captureFilename->hide();
    ui->capturePreviewFrame->hide();
  }

  QString text;

  if(manualReport)
  {
    text =
        tr("<p>Thank you for reporting a problem! Please take a moment to look over this "
           "report to check what has been gathered.</p>");
  }
  else if(replayCrash)
  {
    text =
        tr("<p>RenderDic encountered a serious problem. Please take a moment to look over this "
           "report to check what has been gathered.</p>");
  }
  else
  {
    text =
        tr("<p>A crash happened while RenderDic was injected into your application. It's not "
           "feasible to tell whether the crash was in your application or in RenderDic's capturing "
           "code. The minidump <a href=\"%1\">in the zip</a> might show the problem.</p>")
            .arg(QUrl::fromLocalFile(m_ReportPath).toString());
  }

  if(!m_CaptureFilename.isEmpty() && hasEmbeddedFiles)
    text +=
        tr("<p>Warning: The capture file contains embedded dependency files i.e. shader debug "
           "files.</p>");

  text += tr("<p>The contents of the report can be found <a href=\"%1\">in this zip</a> which "
             "you can edit/censor if you wish.</p>"
             "<p>This zip is temporary and will be deleted when this dialog closes. Use "
             "<b>Save Report As</b> to keep a copy.</p>")
              .arg(QUrl::fromLocalFile(m_ReportPath).toString());

  ui->reportText->setTextFormat(Qt::RichText);
  ui->reportText->setText(text);

  setWindowFlags((windowFlags() | Qt::MSWindowsFixedSizeDialogHint) &
                 ~Qt::WindowContextHelpButtonHint);

  adjustSize();
}

CrashDialog::~CrashDialog()
{
  delete ui;
}

bool CrashDialog::HasCaptureReady(PersistentConfig &cfg)
{
  QFileInfo capInfo(cfg.CrashReport_LastOpenedCapture);

  return capInfo.exists() && capInfo.size() <= MaxUploadSize;
}

bool CrashDialog::CaptureTooLarge(PersistentConfig &cfg)
{
  QFileInfo capInfo(cfg.CrashReport_LastOpenedCapture);

  return capInfo.exists() && capInfo.size() > MaxUploadSize;
}

void CrashDialog::showEvent(QShowEvent *)
{
  adjustSize();
  recentre();
}

void CrashDialog::resizeEvent(QResizeEvent *)
{
  recentre();
}
void CrashDialog::recentre()
{
  QRect scr = QApplication::primaryScreen()->geometry();
  move(scr.center() - rect().center());

  // when we're first shown, on this stage, move the cursor
  if(m_Stage == ReportStage::FillingDetails)
    QCursor::setPos(geometry().center());
}

void CrashDialog::setStage(ReportStage stage)
{
  m_Stage = stage;

  switch(stage)
  {
    case ReportStage::FillingDetails:
      ui->reportGroup->show();
      ui->reportedGroup->hide();
      break;
    case ReportStage::Reported:
      ui->reportGroup->hide();
      ui->reportedGroup->show();
      break;
  }

  adjustSize();
}

void CrashDialog::on_send_clicked()
{
  QString suggested = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
                      lit("/renderdic_report_") +
                      QDateTime::currentDateTime().toString(lit("yyyy.MM.dd.HH.mm.ss")) + lit(".zip");

  QString target = RDDialog::getSaveFileName(this, tr("Save Crash Report"), suggested,
                                             tr("Zip files (*.zip)"));

  if(target.isEmpty())
    return;

  if(QFile::exists(target) && !QFile::remove(target))
  {
    RDDialog::critical(this, tr("Error saving report"),
                       tr("Couldn't replace the existing file %1.").arg(target));
    return;
  }

  if(!QFile::copy(m_ReportPath, target) || !QFileInfo::exists(target))
  {
    RDDialog::critical(this, tr("Error saving report"),
                       tr("Couldn't copy the crash report to %1.").arg(target));
    return;
  }

  ui->finishedText->setTextFormat(Qt::RichText);
  ui->finishedText->setText(
      tr("<p>The crash report has been saved to:</p>"
         "<p><a href=\"%1\">%2</a></p>")
          .arg(QUrl::fromLocalFile(target).toString())
          .arg(target));

  setStage(ReportStage::Reported);
}

void CrashDialog::on_cancel_clicked()
{
  // don't nag the user, just close.
  reject();
}

void CrashDialog::on_buttonBox_accepted()
{
  accept();
}

void CrashDialog::on_captureFilename_linkActivated(const QString &link)
{
  if(QFileInfo::exists(m_CaptureFilename))
    RevealFilenameInExternalFileBrowser(m_CaptureFilename);
}
