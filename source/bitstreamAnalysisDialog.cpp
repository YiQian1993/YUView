/*  This file is part of YUView - The YUV player with advanced analytics toolset
*   <https://github.com/IENT/YUView>
*   Copyright (C) 2015  Institut f�r Nachrichtentechnik, RWTH Aachen University, GERMANY
*
*   This program is free software; you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation; either version 3 of the License, or
*   (at your option) any later version.
*
*   In addition, as a special exception, the copyright holders give
*   permission to link the code of portions of this program with the
*   OpenSSL library under certain conditions as described in each
*   individual source file, and distribute linked combinations including
*   the two.
*   
*   You must obey the GNU General Public License in all respects for all
*   of the code used other than OpenSSL. If you modify file(s) with this
*   exception, you may extend this exception to your version of the
*   file(s), but you are not obligated to do so. If you do not wish to do
*   so, delete this exception statement from your version. If you delete
*   this exception statement from all source files in the program, then
*   also delete it here.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "bitstreamAnalysisDialog.h"

#include "parserAnnexBAVC.h"
#include "parserAnnexBHEVC.h"
#include "parserAnnexBMpeg2.h"
#include "parserAVFormat.h"

#define BITSTREAMANALYSISDIALOG_DEBUG_OUTPUT 0
#if BITSTREAMANALYSISDIALOG_DEBUG_OUTPUT
#include <QDebug>
#define DEBUG_ANALYSIS qDebug
#else
#define DEBUG_ANALYSIS(fmt,...) ((void)0)
#endif

bitstreamAnalysisDialog::bitstreamAnalysisDialog(QWidget *parent, QString fileName, inputFormat inputFormatType) :
  QDialog(parent)
{
  ui.setupUi(this);

  statusBar = new QStatusBar;
  ui.verticalLayout->addWidget(statusBar);

  if (inputFormatType == inputInvalid)
    return;

  // Setup the parser
  if (inputFormatType == inputAnnexBHEVC)
    parser.reset(new parserAnnexBHEVC(this));
  else if (inputFormatType == inputAnnexBAVC)
    parser.reset(new parserAnnexBAVC(this));
  else if (inputFormatType == inputLibavformat)
    parser.reset(new parserAVFormat(this));
  
  parser->enableModel();

  ui.dataTreeView->setModel(parser->getPacketItemModel());

  ui.dataTreeView->setColumnWidth(0, 600);
  ui.dataTreeView->setColumnWidth(1, 100);
  ui.dataTreeView->setColumnWidth(2, 120);

  compressedFilePath = fileName;

  connect(parser.get(), &parserBase::nalModelUpdated, this, &bitstreamAnalysisDialog::updateParserItemModel);
  connect(parser.get(), &parserBase::segmentBitrateListUpdated, this, &bitstreamAnalysisDialog::updateBitrateDisplay);
  connect(parser.get(), &parserBase::streamInfoTextUpdated, this, &bitstreamAnalysisDialog::updateStreamInfoText);
  connect(parser.get(), &parserBase::backgroundParsingDone, this, &bitstreamAnalysisDialog::backgroundParsingDone);
  
  connect(ui.showVideoStreamOnlyCheckBox, &QCheckBox::toggled, this, &bitstreamAnalysisDialog::showVideoStreamOnlyCheckBoxToggled);
  connect(ui.colorCodeStreamsCheckBox, &QCheckBox::toggled, this, &bitstreamAnalysisDialog::colorCodeStreamsCheckBoxToggled);

  // Start the background parsing thread
  statusBar->showMessage("Parsing file ...");
  backgroundParserFuture = QtConcurrent::run(this, &bitstreamAnalysisDialog::backgroundParsingFunction);

  // Add an empty series to the Chart
  QLineSeries* series = new QLineSeries(ui.bitrateGraphicsView);
  ui.bitrateGraphicsView->chart()->setAnimationOptions(QChart::AllAnimations);
  ui.bitrateGraphicsView->chart()->addSeries(series);
  ui.bitrateGraphicsView->chart()->setTitle("Bitrate over time");
  ui.bitrateGraphicsView->chart()->createDefaultAxes();
  QValueAxis *axisX = new QValueAxis();
  ui.bitrateGraphicsView->chart()->setAxisX(axisX, series);

  updateStreamInfoText();
}

bitstreamAnalysisDialog::~bitstreamAnalysisDialog()
{
  // If the background thread is still working, abort it.
  if (backgroundParserFuture.isRunning())
  {
    // signal to background thread that we want to cancel the processing
    parser->setAbortParsing();
    backgroundParserFuture.waitForFinished();
  }
}

void bitstreamAnalysisDialog::updateParserItemModel(unsigned int newNumberItems)
{
  parser->setNewNumberModelItems(newNumberItems);
  statusBar->showMessage(QString("Parsing file (%1%)").arg(parser->getParsingProgressPercent()));
}

void bitstreamAnalysisDialog::updateBitrateDisplay()
{
  // TODO: All streams

  // Add the new data
  QList<QAbstractSeries*> seriesList = ui.bitrateGraphicsView->chart()->series();
  if (seriesList.length() == 0)
    return;

  unsigned int streamIdx = 0;

  QAbstractSeries *series = seriesList[streamIdx];
  QLineSeries *lineSeries = dynamic_cast<QLineSeries*>(series);
  if (!lineSeries)
    return;
  
  QList<parserBase::segmentBitrate> segmentBitrateList = parser->getSegmentBitrateList(streamIdx);
  
  for (int i=addedSegments; i<segmentBitrateList.count(); i++)
  {
    parserBase::segmentBitrate s = segmentBitrateList.at(i);
    // Append a line from start to end
    lineSeries->append(s.startTime, s.bytes);
    lineSeries->append(s.endTime, s.bytes);

    if (s.startTime < rangeAxisX.first)
      rangeAxisX.first = s.startTime;
    if (s.endTime > rangeAxisX.second)
      rangeAxisX.second = s.endTime;
    if ((qint64)s.bytes > rangeAxisY.second)
      rangeAxisY.second = s.bytes;

    DEBUG_ANALYSIS("bitstreamAnalysisDialog::updateBitrateDisplay add horizontal line at y=%d x=(%d,%d)", s.bytes, s.startTime, s.endTime);

    addedSegments++;
  }

  ui.bitrateGraphicsView->chart()->axisX()->setRange(rangeAxisX.first, rangeAxisX.second);
  ui.bitrateGraphicsView->chart()->axisY()->setRange(rangeAxisY.first, rangeAxisY.second);
  DEBUG_ANALYSIS("bitstreamAnalysisDialog::updateBitrateDisplay axis limits (%d, &d) (&d, %d)", rangeAxisX.first, rangeAxisX.second, rangeAxisY.first, rangeAxisY.second);
  DEBUG_ANALYSIS("bitstreamAnalysisDialog::updateBitrateDisplay new segment count %d", lineSeries->count());
}

void bitstreamAnalysisDialog::updateStreamInfoText()
{
  ui.streamInfoText->document()->setPlainText(parser->getStreamInfoText());
}

void bitstreamAnalysisDialog::backgroundParsingDone()
{
  statusBar->showMessage("Parsing done.");
}

void bitstreamAnalysisDialog::backgroundParsingFunction()
{
  parser->runParsingOfFile(compressedFilePath);
}