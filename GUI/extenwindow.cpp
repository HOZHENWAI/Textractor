#include "extenwindow.h"
#include "ui_extenwindow.h"
#include "types.h"
#include <QFileDialog>

namespace
{
	struct InfoForExtension
	{
		const char* name;
		int64_t value;
		InfoForExtension* next;
		~InfoForExtension() { if (next) delete next; };
	};

	QHash<QString, wchar_t*(*)(const wchar_t*, const InfoForExtension*)> extensions;
	QStringList extenNames;
	std::shared_mutex extenMutex;

	void Load(QString extenName)
	{
		// Extension is dll and exports "OnNewSentence"
		HMODULE module = GetModuleHandleW(extenName.toStdWString().c_str());
		if (!module) module = LoadLibraryW(extenName.toStdWString().c_str());
		if (!module) return;
		FARPROC callback = GetProcAddress(module, "OnNewSentence");
		if (!callback) return;
		LOCK(extenMutex);
		extensions[extenName] = (wchar_t*(*)(const wchar_t*, const InfoForExtension*))callback;
		extenNames.push_back(extenName);
	}

	void Unload(QString extenName)
	{
		LOCK(extenMutex);
		extenNames.erase(std::remove(extenNames.begin(), extenNames.end(), extenName), extenNames.end());
		FreeLibrary(GetModuleHandleW(extenName.toStdWString().c_str()));
	}

	void Reorder(QStringList extenNames)
	{
		LOCK(extenMutex);
		::extenNames = extenNames;
	}
}

bool DispatchSentenceToExtensions(std::wstring& sentence, std::unordered_map<std::string, int64_t> miscInfo)
{
	bool success = true;
	wchar_t* sentenceBuffer = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, (sentence.size() + 1) * sizeof(wchar_t));
	wcscpy_s(sentenceBuffer, sentence.size() + 1, sentence.c_str());

	InfoForExtension miscInfoLinkedList{ "", 0, nullptr };
	InfoForExtension* miscInfoTraverser = &miscInfoLinkedList;
	for (auto& i : miscInfo) miscInfoTraverser = miscInfoTraverser->next = new InfoForExtension{ i.first.c_str(), i.second, nullptr };

	std::shared_lock sharedLock(extenMutex);
	for (auto extenName : extenNames)
	{
		wchar_t* nextBuffer = extensions[extenName](sentenceBuffer, &miscInfoLinkedList);
		if (nextBuffer == nullptr) { success = false; break; }
		if (nextBuffer != sentenceBuffer) HeapFree(GetProcessHeap(), 0, sentenceBuffer);
		sentenceBuffer = nextBuffer;
	}
	sentence = std::wstring(sentenceBuffer);

	HeapFree(GetProcessHeap(), 0, sentenceBuffer);
	return success;
}

ExtenWindow::ExtenWindow(QWidget* parent) :
	QMainWindow(parent),
	ui(new Ui::ExtenWindow)
{
	ui->setupUi(this);

	extenList = findChild<QListWidget*>("extenList");
	extenList->installEventFilter(this);

	if (extensions.empty())
	{
		extenSaveFile.open(QIODevice::ReadOnly);
		for (auto extenName : QString(extenSaveFile.readAll()).split(">")) Load(extenName);
		extenSaveFile.close();
	}
	Sync();
}

ExtenWindow::~ExtenWindow()
{
	delete ui;
}

void ExtenWindow::on_addButton_clicked()
{
	QString extenFileName = QFileDialog::getOpenFileName(this, "Select Extension", "C:\\", "Extensions (*.dll)");
	if (!extenFileName.size()) return;
	QString extenName = extenFileName.mid(extenFileName.lastIndexOf("/") + 1);
	QFile::copy(extenFileName, extenName);
	Load(extenName.left(extenName.lastIndexOf(".dll")));
	Sync();
}

void ExtenWindow::on_rmvButton_clicked()
{
	if (auto extenName = extenList->currentItem()) Unload(extenName->text());
	Sync();
}

bool ExtenWindow::eventFilter(QObject* target, QEvent* event) 
{ 
	// See https://stackoverflow.com/questions/1224432/how-do-i-respond-to-an-internal-drag-and-drop-operation-using-a-qlistwidget/1528215
	if (event->type() == QEvent::ChildRemoved)
	{
		QStringList extenNames;
		for (int i = 0; i < extenList->count(); ++i) extenNames.push_back(extenList->item(i)->text());
		Reorder(extenNames);
		Sync();
	}
	return false; 
}

void ExtenWindow::Sync()
{
	extenList->clear();
	extenSaveFile.open(QIODevice::WriteOnly | QIODevice::Truncate);
	std::shared_lock sharedLock(extenMutex);
	for (auto extenName : extenNames)
	{
		extenList->addItem(extenName);
		extenSaveFile.write((extenName + ">").toUtf8());
	}
	extenSaveFile.close();
}
