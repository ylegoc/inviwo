/*********************************************************************************
 *
 * Inviwo - Interactive Visualization Workshop
 *
 * Copyright (c) 2012-2023 Inviwo Foundation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *********************************************************************************/

#include <warn/push>
#include <warn/ignore/all>
#include <QMenu>
#include <QTextCursor>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTableView>
#include <QKeyEvent>
#include <QLabel>
#include <QThread>
#include <QCoreApplication>
#include <QFontDatabase>
#include <QHeaderView>
#include <QStandardItem>
#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QSortFilterProxyModel>
#include <QLineEdit>
#include <QToolButton>
#include <QPixmap>
#include <QSettings>
#include <QScrollBar>
#include <QResizeEvent>
#include <QWheelEvent>
#include <QMessageBox>
#include <QTimer>
#include <QPlainTextEdit>
#include <warn/pop>

#include <inviwo/core/common/inviwoapplication.h>
#include <inviwo/qt/editor/consolewidget.h>
#include <inviwo/core/util/filesystem.h>
#include <inviwo/core/util/stringconversion.h>
#include <inviwo/core/processors/processor.h>
#include <inviwo/qt/editor/inviwomainwindow.h>
#include <inviwo/core/util/ostreamjoiner.h>
#include <inviwo/qt/editor/inviwoeditmenu.h>

#include <inviwo/core/network/processornetwork.h>
#include <inviwo/core/network/processornetworkobserver.h>

namespace inviwo {

namespace detail {

enum Roles { Fulltext = Qt::UserRole + 1 };

}  // namespace detail

TextSelectionDelegate::TextSelectionDelegate(QWidget* parent) : QItemDelegate(parent) {}

QWidget* TextSelectionDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                                             const QModelIndex& index) const {
    if (index.column() == static_cast<int>(LogTableModelEntry::ColumnID::Message)) {
        auto value = index.model()->data(index, Qt::EditRole).toString();
        auto widget = new QPlainTextEdit(value, parent);
        widget->setReadOnly(true);
        return widget;
    } else {
        return QItemDelegate::createEditor(parent, option, index);
    }
}

void TextSelectionDelegate::setModelData([[maybe_unused]] QWidget* editor,
                                         [[maybe_unused]] QAbstractItemModel* model,
                                         [[maybe_unused]] const QModelIndex& index) const {
    // dummy function to prevent changing the model
}

struct BackgroundJobs : QLabel, ProcessorNetworkObserver {
    BackgroundJobs(QWidget* parent, ProcessorNetwork* net) : QLabel(parent) {
        net->addObserver(this);
        update(0);
    }

    void update(int jobs) { setText(QString("Backgrund Jobs: %1").arg(jobs)); }

    virtual void onProcessorBackgroundJobsChanged(Processor*, int, int total) override {
        update(total);
    }
};

ConsoleWidget::ConsoleWidget(InviwoMainWindow* parent)
    : InviwoDockWidget(tr("Console"), parent, "ConsoleWidget")
    , tableView_(new QTableView(this))
    , model_()
    , filter_(new QSortFilterProxyModel(this))
    , levelFilter_(new QSortFilterProxyModel(this))
    , textSelectionDelegate_(new TextSelectionDelegate(this))
    , filterPattern_(new QLineEdit(this))
    , mainwindow_(parent)
    , editActionsHandle_{} {

    setAllowedAreas(Qt::BottomDockWidgetArea);
    resize(utilqt::emToPx(this, QSizeF(60, 60)));  // default size

    qRegisterMetaType<LogTableModelEntry>("LogTableModelEntry");

    filter_->setSourceModel(model_.model());
    filter_->setFilterKeyColumn(static_cast<int>(LogTableModelEntry::ColumnID::Message));

    levelFilter_->setSourceModel(filter_);
    levelFilter_->setFilterKeyColumn(static_cast<int>(LogTableModelEntry::ColumnID::Level));

    filterPattern_->setClearButtonEnabled(true);

    tableView_->setModel(levelFilter_);
    tableView_->setGridStyle(Qt::NoPen);
    tableView_->setCornerButtonEnabled(false);

    tableView_->setContextMenuPolicy(Qt::ActionsContextMenu);
    clearAction_ = new QAction(QIcon(":/svgicons/log-clear.svg"), tr("&Clear Log"), this);
    clearAction_->setShortcut(Qt::CTRL | Qt::Key_E);
    connect(clearAction_, &QAction::triggered, [&]() { clear(); });

    tableView_->hideColumn(static_cast<int>(LogTableModelEntry::ColumnID::Date));
    tableView_->hideColumn(static_cast<int>(LogTableModelEntry::ColumnID::Level));
    tableView_->hideColumn(static_cast<int>(LogTableModelEntry::ColumnID::Audience));
    tableView_->hideColumn(static_cast<int>(LogTableModelEntry::ColumnID::Path));
    tableView_->hideColumn(static_cast<int>(LogTableModelEntry::ColumnID::File));
    tableView_->hideColumn(static_cast<int>(LogTableModelEntry::ColumnID::Line));
    tableView_->hideColumn(static_cast<int>(LogTableModelEntry::ColumnID::Function));

    tableView_->horizontalHeader()->setContextMenuPolicy(Qt::ActionsContextMenu);
    tableView_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    const auto cols = tableView_->horizontalHeader()->count();

    auto viewColGroup = new QMenu(this);
    for (int i = 0; i < cols; ++i) {
        auto viewCol =
            new QAction(model_.getName(static_cast<LogTableModelEntry::ColumnID>(i)), this);
        viewCol->setCheckable(true);
        viewCol->setChecked(!tableView_->isColumnHidden(i));
        connect(viewCol, &QAction::triggered, [this, i](bool state) {
            if (!state) {
                tableView_->hideColumn(i);
            } else {
                tableView_->showColumn(i);
            }
        });
        tableView_->horizontalHeader()->addAction(viewCol);
        viewColGroup->addAction(viewCol);
    }
    auto visibleColumnsAction = new QAction("Visible Columns", this);
    visibleColumnsAction->setMenu(viewColGroup);

    tableView_->horizontalHeader()->setResizeContentsPrecision(0);
    tableView_->horizontalHeader()->setSectionResizeMode(cols - 1, QHeaderView::Stretch);
    tableView_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);

    tableView_->verticalHeader()->setVisible(false);
    tableView_->verticalHeader()->setResizeContentsPrecision(0);
    tableView_->verticalHeader()->setMinimumSectionSize(1);
    tableView_->verticalHeader()->setDefaultSectionSize(1);
    tableView_->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);

    QHBoxLayout* statusBar = new QHBoxLayout();
    statusBar->setObjectName("StatusBar");

    auto makeIcon = [](const QString& file, bool checkable = false) {
        auto icon = QIcon();
        if (checkable) {
            icon.addPixmap(QPixmap(":/svgicons/" + file + "-enabled.svg"), QIcon::Normal,
                           QIcon::On);
            icon.addPixmap(QPixmap(":/svgicons/" + file + "-disabled.svg"), QIcon::Normal,
                           QIcon::Off);
        } else {
            icon.addPixmap(QPixmap(":/svgicons/" + file + ".svg"));
        }
        return icon;
    };

    auto makeToolButton = [this, statusBar, makeIcon](const QString& label, const QString& file,
                                                      bool checkable = true) {
        auto button = new QToolButton(this);
        auto action = new QAction(makeIcon(file, checkable), label, this);
        action->setCheckable(checkable);
        if (checkable) action->setChecked(true);

        button->setDefaultAction(action);
        statusBar->addWidget(button);
        return action;
    };

    auto levelCallback = [this](bool /*checked*/) {
        if (util::all_of(levels, [](const auto& level) { return level.action->isChecked(); })) {
            levelFilter_->setFilterRegularExpression("");
        } else {
            std::stringstream ss;
            auto joiner = util::make_ostream_joiner(ss, "|");
            joiner = "None";
            for (const auto& level : levels) {
                if (level.action->isChecked()) joiner = level.level;
            }
            levelFilter_->setFilterRegularExpression(QString::fromStdString(ss.str()));
        }
    };

    auto levelGroup = new QMenu(this);
    for (auto& level : levels) {
        level.action =
            makeToolButton(QString::fromStdString(level.name), QString::fromStdString(level.icon));
        level.label = new QLabel("0", this);
        statusBar->addWidget(level.label);
        statusBar->addSpacing(5);
        levelGroup->addAction(level.action);
        connect(level.action, &QAction::toggled, levelCallback);
    }
    auto viewAction = new QAction("Log Level", this);
    viewAction->setMenu(levelGroup);

    auto clearButton = new QToolButton(this);
    clearButton->setDefaultAction(clearAction_);
    statusBar->addWidget(clearButton);
    statusBar->addSpacing(5);

    statusBar->addStretch(3);

    threadPoolInfo_ = new QLabel("Pool: 0 Queued Jobs / 0 Threads", this);
    statusBar->addWidget(threadPoolInfo_);
    auto timer = new QTimer(this);
    connect(timer, &QTimer::timeout, threadPoolInfo_, [this]() {
        const auto threads = mainwindow_->getInviwoApplication()->getThreadPool().getSize();
        const auto queueSize = mainwindow_->getInviwoApplication()->getThreadPool().getQueueSize();
        threadPoolInfo_->setText(
            QString("Pool: %1 Queued Jobs / %2 Threads").arg(queueSize, 3).arg(threads, 2));
    });
    timer->start(1000);

    statusBar->addWidget(
        new BackgroundJobs(this, mainwindow_->getInviwoApplication()->getProcessorNetwork()));

    statusBar->addSpacing(20);
    statusBar->addWidget(new QLabel("Filter", this));
    filterPattern_->setMinimumWidth(200);
    statusBar->addWidget(filterPattern_, 1);
    statusBar->addSpacing(5);

    auto clearFilter = new QAction(makeIcon("find-clear"), "C&lear Filter", this);
    clearFilter->setEnabled(false);

    connect(filterPattern_, &QLineEdit::textChanged, [this, clearFilter](const QString& text) {
        filter_->setFilterRegularExpression(text);
        clearFilter->setEnabled(!text.isEmpty());
    });

    connect(clearFilter, &QAction::triggered, [this]() { filterPattern_->setText(""); });

    auto filterAction = new QAction(makeIcon("find"), "&Filter", this);
    filterAction->setShortcut(Qt::CTRL | Qt::ALT | Qt::Key_F);
    connect(filterAction, &QAction::triggered, [this]() {
        raise();
        filterPattern_->setFocus();
        filterPattern_->selectAll();
    });

    // add actions for context menu
    auto createSeparator = [this]() {
        auto separator = new QAction(this);
        separator->setSeparator(true);
        return separator;
    };

    auto copyAction = new QAction(QIcon(":/svgicons/edit-copy.svg"), tr("&Copy"), this);
    copyAction->setEnabled(true);
    connect(copyAction, &QAction::triggered, this, &ConsoleWidget::copy);

    tableView_->addAction(copyAction);
    tableView_->addAction(createSeparator());
    tableView_->addAction(visibleColumnsAction);
    tableView_->addAction(viewAction);
    tableView_->addAction(createSeparator());
    tableView_->addAction(clearAction_);
    tableView_->addAction(createSeparator());
    tableView_->addAction(filterAction);
    tableView_->addAction(clearFilter);

    QVBoxLayout* layout = new QVBoxLayout();
    layout->addWidget(tableView_);
    layout->addLayout(statusBar);

    const auto space = utilqt::emToPx(this, 3 / 9.0);
    layout->setContentsMargins(space, 0, 0, space);

    QWidget* w = new QWidget();
    w->setLayout(layout);
    setWidget(w);

    tableView_->setAttribute(Qt::WA_Hover);
    tableView_->setItemDelegateForColumn(static_cast<int>(LogTableModelEntry::ColumnID::Message),
                                         textSelectionDelegate_);

    connect(this, &ConsoleWidget::logSignal, this, &ConsoleWidget::logEntry);
    connect(this, &ConsoleWidget::clearSignal, this, &ConsoleWidget::clear);

    // Restore State
    QSettings settings;
    settings.beginGroup(objectName());

    {
        auto colVisible = settings.value("columnsVisible", QVariantList()).toList();
        auto colWidths = settings.value("columnsWidth", QVariantList()).toList();
        auto count = std::min(colVisible.size(), colWidths.size());

        for (int i = 0; i < count; ++i) {
            const bool visible = colVisible[i].toBool();
            viewColGroup->actions()[i]->setChecked(visible);
            tableView_->horizontalHeader()->setSectionHidden(i, !visible);
            if (visible) tableView_->horizontalHeader()->resizeSection(i, colWidths[i].toInt());
        }
    }

    {
        auto levelsActive = settings.value("levelsActive", QVariantList());
        int i = 0;
        for (const auto& level : levelsActive.toList()) {
            levels[i++].action->setChecked(level.toBool());
        }
    }

    auto filterText = settings.value("filterText", "");
    filterPattern_->setText(filterText.toString());

    settings.endGroup();

    auto editmenu = mainwindow_->getInviwoEditMenu();
    editActionsHandle_ = editmenu->registerItem(std::make_shared<MenuItem>(
        this,
        [this](MenuItemType t) -> bool {
            switch (t) {
                case MenuItemType::copy:
                    return tableView_->selectionModel()->hasSelection();
                case MenuItemType::cut:
                case MenuItemType::paste:
                case MenuItemType::del:
                case MenuItemType::select:
                default:
                    return false;
            }
        },
        [this](MenuItemType t) -> void {
            switch (t) {
                case MenuItemType::copy: {
                    if (tableView_->selectionModel()->hasSelection()) {
                        copy();
                    }
                    break;
                }
                case MenuItemType::cut:
                case MenuItemType::paste:
                case MenuItemType::del:
                case MenuItemType::select:
                default:
                    break;
            }
        }));
}

ConsoleWidget::~ConsoleWidget() = default;

QAction* ConsoleWidget::getClearAction() { return clearAction_; }

void ConsoleWidget::clear() {
    if (QThread::currentThread() != QCoreApplication::instance()->thread()) {
        emit clearSignal();
        return;
    }

    model_.clear();
    for (auto& level : levels) {
        level.label->setText("0");
        level.count = 0;
    }
}

void ConsoleWidget::updateIndicators(LogLevel level) {
    auto it = util::find_if(levels, [&](const auto& l) { return l.level == level; });
    if (it != levels.end()) {
        it->label->setText(toString(++(it->count)).c_str());
    }
}

void ConsoleWidget::log(std::string_view source, LogLevel level, LogAudience audience,
                        std::string_view file, std::string_view function, int line,
                        std::string_view msg) {
    LogTableModelEntry e{
        std::chrono::system_clock::now(), source, level, audience, file, line, function, msg};
    logEntry(std::move(e));
}

void ConsoleWidget::logProcessor(Processor* processor, LogLevel level, LogAudience audience,
                                 std::string_view msg, std::string_view file,
                                 std::string_view function, int line) {
    LogTableModelEntry e{std::chrono::system_clock::now(),
                         processor->getIdentifier(),
                         level,
                         audience,
                         file,
                         line,
                         function,
                         msg};
    logEntry(std::move(e));
}

void ConsoleWidget::logNetwork(LogLevel level, LogAudience audience, std::string_view msg,
                               std::string_view file, std::string_view function, int line) {
    LogTableModelEntry e{std::chrono::system_clock::now(),
                         "ProcessorNetwork",
                         level,
                         audience,
                         file,
                         line,
                         function,
                         msg};
    logEntry(std::move(e));
}

void ConsoleWidget::logAssertion(std::string_view file, std::string_view function, int line,
                                 std::string_view msg) {
    LogTableModelEntry e{std::chrono::system_clock::now(),
                         "Assertion",
                         LogLevel::Error,
                         LogAudience::Developer,
                         file,
                         line,
                         function,
                         msg};
    logEntry(std::move(e));

    auto error = QString{"<b>Assertion Failed</b><br>File: %1:%2<br>Function: %3<p>%4"}
                     .arg(file.data())
                     .arg(line)
                     .arg(function.data())
                     .arg(utilqt::toQString(msg));
    QMessageBox::critical(nullptr, "Assertion Failed", error);
}

void ConsoleWidget::logEntry(LogTableModelEntry e) {
    if (QThread::currentThread() != QCoreApplication::instance()->thread()) {
        emit logSignal(e);
        return;
    }

    tableView_->setUpdatesEnabled(false);
    updateIndicators(e.level);
    model_.log(std::move(e));
    tableView_->scrollToBottom();
    tableView_->setUpdatesEnabled(true);
}

void ConsoleWidget::keyPressEvent(QKeyEvent* keyEvent) {
    if (keyEvent->key() == Qt::Key_E && keyEvent->modifiers() == Qt::ControlModifier) {
        clear();
    }
}

QModelIndex ConsoleWidget::mapToSource(int row, int col) {
    auto ind = levelFilter_->index(row, col);
    auto lind = levelFilter_->mapToSource(ind);
    return filter_->mapToSource(lind);
}

QModelIndex ConsoleWidget::mapFromSource(int row, int col) {
    auto mind = model_.model()->index(row, col);
    auto lind = filter_->mapFromSource(mind);
    return levelFilter_->mapFromSource(lind);
}

void ConsoleWidget::copy() {
    const auto& inds = tableView_->selectionModel()->selectedIndexes();
    if (inds.isEmpty()) return;

    int prevrow = inds.first().row();
    bool first = true;
    QString text;
    for (const auto& ind : inds) {
        if (!tableView_->isColumnHidden(ind.column())) {
            if (!first && ind.row() == prevrow) {
                text.append('\t');
            } else if (!first) {
                text.append('\n');
            }
            if (auto v = ind.data(detail::Roles::Fulltext); !v.isNull()) {
                text.append(v.toString());
            } else {
                text.append(ind.data(Qt::DisplayRole).toString());
            }
            first = false;
        }
        prevrow = ind.row();
    }
    auto mimedata = std::make_unique<QMimeData>();
    mimedata->setData(QString("text/plain"), text.toUtf8());
    QApplication::clipboard()->setMimeData(mimedata.release());
}

void ConsoleWidget::closeEvent(QCloseEvent* event) {
    QSettings settings;
    settings.beginGroup(objectName());

    const auto cols = tableView_->horizontalHeader()->count();
    QList<QVariant> columnsVisible;
    QList<QVariant> columnsWidth;
    columnsVisible.reserve(cols);
    columnsWidth.reserve(cols);
    for (int i = 0; i < cols; ++i) {
        columnsVisible.append(!tableView_->horizontalHeader()->isSectionHidden(i));
        columnsWidth.append(tableView_->horizontalHeader()->sectionSize(i));
    }
    QList<QVariant> levelsActive;
    for (const auto& level : levels) {
        levelsActive.append(level.action->isChecked());
    }

    settings.setValue("columnsVisible", QVariant(columnsVisible));
    settings.setValue("columnsWidth", QVariant(columnsWidth));
    settings.setValue("levelsActive", QVariant(levelsActive));
    settings.setValue("filterText", QVariant(filterPattern_->text()));
    settings.endGroup();

    InviwoDockWidget::closeEvent(event);
}

namespace {
std::pair<int, int> getLineHeightAndMargin(const QFont& font) {
    QStyleOptionViewItem opt;
    opt.font = font;
    opt.fontMetrics = QFontMetrics{font};
    opt.features |= QStyleOptionViewItem::HasDisplay;
    opt.styleObject = nullptr;
    opt.text = "One line text";
    auto* style = qApp->style();
    auto size1 = style->sizeFromContents(QStyle::CT_ItemViewItem, &opt, QSize(), nullptr);
    opt.text = "One line text\nAnother line";
    opt.text.replace(QLatin1Char('\n'), QChar::LineSeparator);
    auto size2 = style->sizeFromContents(QStyle::CT_ItemViewItem, &opt, QSize(), nullptr);

    int lineHeight = size2.height() - size1.height();
    int margin = size1.height() - lineHeight;

    return {lineHeight, margin};
}

}  // namespace

LogTableModel::LogTableModel() : model_(0, static_cast<int>(LogTableModelEntry::size())) {

    for (size_t i = 0; i < LogTableModelEntry::size(); ++i) {
        auto item = new QStandardItem(getName(static_cast<LogTableModelEntry::ColumnID>(i)));
        item->setTextAlignment(Qt::AlignLeft);
        model_.setHorizontalHeaderItem(static_cast<int>(i), item);
    }
}

void LogTableModel::log(LogTableModelEntry entry) {
    model_.appendRow(entry.items());
    model_.setVerticalHeaderItem(model_.rowCount() - 1, entry.header());
}

LogModel* LogTableModel::model() { return &model_; }

void LogTableModel::clear() { model_.removeRows(0, model_.rowCount()); }

QString LogTableModel::getName(LogTableModelEntry::ColumnID ind) const {
    switch (ind) {
        case LogTableModelEntry::ColumnID::Date:
            return QString("Date");
        case LogTableModelEntry::ColumnID::Time:
            return QString("Time");
        case LogTableModelEntry::ColumnID::Source:
            return QString("Source");
        case LogTableModelEntry::ColumnID::Level:
            return QString("Level");
        case LogTableModelEntry::ColumnID::Audience:
            return QString("Audience");
        case LogTableModelEntry::ColumnID::Path:
            return QString("Path");
        case LogTableModelEntry::ColumnID::File:
            return QString("File");
        case LogTableModelEntry::ColumnID::Line:
            return QString("Line");
        case LogTableModelEntry::ColumnID::Function:
            return QString("Function");
        case LogTableModelEntry::ColumnID::Message:
            return QString("Message");
        default:
            return QString();
    }
}

const QFont& LogTableModelEntry::logFont() {
    static QFont font{QFontDatabase::systemFont(QFontDatabase::FixedFont)};
    return font;
}

const std::pair<int, int>& LogTableModelEntry::lineHeightAndMargin() {
    static std::pair<int, int> lineAndMargin{getLineHeightAndMargin(logFont())};
    return lineAndMargin;
}

LogTableModelEntry::LogTableModelEntry(std::chrono::system_clock::time_point time,
                                       std::string_view source, LogLevel level,
                                       LogAudience audience, const std::filesystem::path& file,
                                       int line, std::string_view function, std::string_view msg)

    : level{level}
    , header_(new QStandardItem())
    , date_{new QStandardItem(utilqt::toQString(getDate(time)))}
    , time_{new QStandardItem(utilqt::toQString(getTime(time)))}
    , source_{new QStandardItem(utilqt::toQString(source))}
    , level_{new QStandardItem(utilqt::toQString(toString(level)))}
    , audience_{new QStandardItem(utilqt::toQString(toString(audience)))}
    , path_{new QStandardItem(utilqt::toQString(file.parent_path()))}
    , file_{new QStandardItem(utilqt::toQString(file.filename()))}
    , line_{new QStandardItem(utilqt::toQString(toString(line)))}
    , function_{new QStandardItem(utilqt::toQString(function))}
    , message_{new QStandardItem()} {

    msg = util::rtrim(msg);
    message_->setData(utilqt::toQString(msg), detail::Roles::Fulltext);
    message_->setData(utilqt::toQString(util::elideLines(msg)), Qt::DisplayRole);

    const auto lines = std::count(msg.begin(), msg.end(), '\n') + 1;
    const auto [lineHeight, margin] = lineHeightAndMargin();
    header_->setSizeHint(QSize(1, static_cast<int>(margin + lines * lineHeight)));

    QColor infoTextColor = {153, 153, 153};
    QColor warnTextColor = {221, 165, 8};
    QColor errorTextColor = {255, 107, 107};

    for (auto& item :
         {date_, time_, source_, level_, audience_, path_, file_, line_, function_, message_}) {
        item->setFont(logFont());
        item->setTextAlignment(Qt::AlignLeft);
        item->setEditable(false);
        switch (level) {
            case LogLevel::Info:
                item->setForeground(QBrush(infoTextColor));
                break;
            case LogLevel::Warn:
                item->setForeground(QBrush(warnTextColor));
                break;
            case LogLevel::Error:
                item->setForeground(QBrush(errorTextColor));
                break;
            default:
                item->setForeground(QBrush(infoTextColor));
                break;
        }
    }
}

std::string LogTableModelEntry::getDate(std::chrono::system_clock::time_point time) {
    auto in_time_t = std::chrono::system_clock::to_time_t(time);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%F");
    return std::move(ss).str();
}
std::string LogTableModelEntry::getTime(std::chrono::system_clock::time_point time) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()) % 1000;

    auto in_time_t = std::chrono::system_clock::to_time_t(time);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%T");
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return std::move(ss).str();
}

QList<QStandardItem*> LogTableModelEntry::items() {
    return QList<QStandardItem*>{date_, time_, source_, level_,    audience_,
                                 path_, file_, line_,   function_, message_};
}

QStandardItem* LogTableModelEntry::header() { return header_; }

LogModel::LogModel(int rows, int columns, QObject* parent)
    : QStandardItemModel(rows, columns, parent) {}

Qt::ItemFlags LogModel::flags(const QModelIndex& index) const {
    auto flags = QStandardItemModel::flags(index);
    // make only the message column editable
    const auto col = static_cast<LogTableModelEntry::ColumnID>(index.column());
    if (col == LogTableModelEntry::ColumnID::Message) {
        flags |= Qt::ItemIsEditable;
    }
    return flags;
}

}  // namespace inviwo
