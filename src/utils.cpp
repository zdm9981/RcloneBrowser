#include "utils.h"
#include <QRadioButton>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QSpinBox>
#include <QLineEdit>
#include <QPlainTextEdit>

static QString gRclone;
static QString gRcloneConf;
static QString gRclonePassword;

namespace {

constexpr const char *kLocalHostsFileName = "hosts";

QString localHostsHeaderValue(const QUrl &url) {
  QString host = url.host();
  if (url.port() != -1) {
    host += ":" + QString::number(url.port());
  }
  return host;
}

QHash<QString, QString> readLocalHosts() {
  QHash<QString, QString> hosts;
  QFile file(GetLocalHostsPath());
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return hosts;
  }

  while (!file.atEnd()) {
    QString line = QString::fromUtf8(file.readLine());
    const int commentIndex = line.indexOf('#');
    if (commentIndex >= 0) {
      line = line.left(commentIndex);
    }
    line = line.trimmed();
    if (line.isEmpty()) {
      continue;
    }

    const QStringList parts =
        line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    if (parts.size() < 2) {
      continue;
    }

    QHostAddress address;
    if (!address.setAddress(parts.first())) {
      continue;
    }

    for (int i = 1; i < parts.size(); ++i) {
      hosts.insert(parts.at(i).toLower(), parts.first());
    }
  }

  return hosts;
}

class LocalHostsNetworkAccessManager : public QNetworkAccessManager {
public:
  explicit LocalHostsNetworkAccessManager(QObject *parent = nullptr)
      : QNetworkAccessManager(parent) {}

protected:
  QNetworkReply *createRequest(Operation op, const QNetworkRequest &request,
                               QIODevice *outgoingData = nullptr) override {
    QNetworkRequest mappedRequest(request);
    QUrl url = mappedRequest.url();
    const QString originalHost = url.host();
    const QString mappedAddress =
        readLocalHosts().value(originalHost.toLower());

    if (!mappedAddress.isEmpty()) {
      mappedRequest.setRawHeader("Host",
                                 localHostsHeaderValue(url).toUtf8());
      url.setHost(mappedAddress);
      mappedRequest.setUrl(url);

      if (url.scheme().compare("https", Qt::CaseInsensitive) == 0) {
        mappedRequest.setPeerVerifyName(originalHost);
      }
    }

    return QNetworkAccessManager::createRequest(op, mappedRequest,
                                                outgoingData);
  }
};

class LocalHostsProxyConnection : public QObject {
public:
  explicit LocalHostsProxyConnection(qintptr socketDescriptor, QObject *parent)
      : QObject(parent), mClient(new QTcpSocket(this)) {
    if (!mClient->setSocketDescriptor(socketDescriptor)) {
      deleteLater();
      return;
    }

    QObject::connect(mClient, &QTcpSocket::readyRead, mClient,
                     [this]() { readClient(); });
    QObject::connect(mClient, &QTcpSocket::disconnected, this,
                     &QObject::deleteLater);
  }

private:
  QTcpSocket *mClient = nullptr;
  QTcpSocket *mUpstream = nullptr;
  QByteArray mRequestBuffer;

  void readClient() {
    if (mUpstream) {
      mUpstream->write(mClient->readAll());
      return;
    }

    mRequestBuffer += mClient->readAll();
    const int headerEnd = mRequestBuffer.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
      return;
    }

    const QByteArray firstLine = mRequestBuffer.left(mRequestBuffer.indexOf("\r\n"));
    const QList<QByteArray> parts = firstLine.split(' ');
    if (parts.size() < 3 || parts.at(0).toUpper() != "CONNECT") {
      mClient->write("HTTP/1.1 501 Not Implemented\r\n\r\n");
      mClient->disconnectFromHost();
      return;
    }

    const QByteArray target = parts.at(1);
    const int portSeparator = target.lastIndexOf(':');
    if (portSeparator <= 0) {
      mClient->write("HTTP/1.1 400 Bad Request\r\n\r\n");
      mClient->disconnectFromHost();
      return;
    }

    const QString host = QString::fromUtf8(target.left(portSeparator));
    bool ok = false;
    const quint16 port = target.mid(portSeparator + 1).toUShort(&ok);
    if (!ok) {
      mClient->write("HTTP/1.1 400 Bad Request\r\n\r\n");
      mClient->disconnectFromHost();
      return;
    }

    const QString mappedHost = readLocalHosts().value(host.toLower(), host);
    const QByteArray bodyAfterHeaders = mRequestBuffer.mid(headerEnd + 4);

    mUpstream = new QTcpSocket(this);
    QObject::connect(mUpstream, &QTcpSocket::connected, mUpstream,
                     [this, bodyAfterHeaders]() {
                       mClient->write("HTTP/1.1 200 Connection Established\r\n\r\n");
                       if (!bodyAfterHeaders.isEmpty()) {
                         mUpstream->write(bodyAfterHeaders);
                       }
                     });
    QObject::connect(mUpstream, &QTcpSocket::readyRead, mUpstream,
                     [this]() { mClient->write(mUpstream->readAll()); });
    QObject::connect(mUpstream, &QTcpSocket::disconnected, mClient,
                     &QTcpSocket::disconnectFromHost);
    QObject::connect(mUpstream, &QTcpSocket::errorOccurred, mClient,
                     [this]() { mClient->disconnectFromHost(); });
    QObject::connect(mClient, &QTcpSocket::disconnected, mUpstream,
                     &QTcpSocket::disconnectFromHost);

    mUpstream->connectToHost(mappedHost, port);
  }
};

class LocalHostsProxyServer : public QTcpServer {
public:
  explicit LocalHostsProxyServer(QObject *parent = nullptr)
      : QTcpServer(parent) {}

protected:
  void incomingConnection(qintptr socketDescriptor) override {
    new LocalHostsProxyConnection(socketDescriptor, this);
  }
};

LocalHostsProxyServer *gLocalHostsProxyServer = nullptr;

QString localHostsProxyUrl() {
  EnsureLocalHostsFile();

  if (readLocalHosts().isEmpty()) {
    return QString();
  }

  if (!gLocalHostsProxyServer) {
    gLocalHostsProxyServer = new LocalHostsProxyServer(qApp);
  }

  if (!gLocalHostsProxyServer->isListening() &&
      !gLocalHostsProxyServer->listen(QHostAddress::LocalHost, 0)) {
    return QString();
  }

  return QString("http://127.0.0.1:%1")
      .arg(gLocalHostsProxyServer->serverPort());
}

} // namespace

QString GetLocalHostsPath() {
  const QString currentPath = QDir::current().filePath(kLocalHostsFileName);
  if (QFileInfo::exists(currentPath)) {
    return currentPath;
  }

  return QDir(qApp->applicationDirPath()).filePath(kLocalHostsFileName);
}

void EnsureLocalHostsFile() {
  QFile file(GetLocalHostsPath());
  if (file.exists()) {
    return;
  }

  if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
    file.close();
  }
}

QNetworkAccessManager *CreateNetworkAccessManager(QObject *parent) {
  EnsureLocalHostsFile();
  return new LocalHostsNetworkAccessManager(parent);
}

QString GetOpenFileNameNative(QWidget *parent, const QString &caption,
                              const QString &dir, const QString &filter) {
  QFileDialog dialog(parent, caption, dir, filter);
  dialog.setFileMode(QFileDialog::ExistingFile);
  dialog.setAcceptMode(QFileDialog::AcceptOpen);
#ifdef Q_OS_WIN
  dialog.setOption(QFileDialog::DontUseNativeDialog, false);
#endif
  if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) {
    return QString();
  }
  return dialog.selectedFiles().first();
}

QStringList GetOpenFileNamesNative(QWidget *parent, const QString &caption,
                                   const QString &dir, const QString &filter) {
  QFileDialog dialog(parent, caption, dir, filter);
  dialog.setFileMode(QFileDialog::ExistingFiles);
  dialog.setAcceptMode(QFileDialog::AcceptOpen);
#ifdef Q_OS_WIN
  dialog.setOption(QFileDialog::DontUseNativeDialog, false);
#endif
  if (dialog.exec() != QDialog::Accepted) {
    return QStringList();
  }
  return dialog.selectedFiles();
}

QString GetSaveFileNameNative(QWidget *parent, const QString &caption,
                              const QString &dir, const QString &filter) {
  QFileDialog dialog(parent, caption, dir, filter);
  dialog.setFileMode(QFileDialog::AnyFile);
  dialog.setAcceptMode(QFileDialog::AcceptSave);
#ifdef Q_OS_WIN
  dialog.setOption(QFileDialog::DontUseNativeDialog, false);
#endif
  if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) {
    return QString();
  }
  return dialog.selectedFiles().first();
}

QString GetExistingDirectoryNative(QWidget *parent, const QString &caption,
                                   const QString &dir) {
  QFileDialog dialog(parent, caption, dir);
  dialog.setFileMode(QFileDialog::Directory);
  dialog.setOption(QFileDialog::ShowDirsOnly, true);
  dialog.setAcceptMode(QFileDialog::AcceptOpen);
#ifdef Q_OS_WIN
  dialog.setOption(QFileDialog::DontUseNativeDialog, false);
#endif
  if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) {
    return QString();
  }
  return dialog.selectedFiles().first();
}

// Software versions comparison
// source: https://helloacm.com/how-to-compare-version-numbers-in-c/
std::vector<std::string> split(const std::string &s, char d) {
  std::vector<std::string> r;
  int j = 0;
  for (unsigned int i = 0; i < s.length(); i++) {
    if (s[i] == d) {
      r.push_back(s.substr(j, i - j));
      j = i + 1;
    }
  }
  r.push_back(s.substr(j));
  return r;
}

unsigned int compareVersion(std::string version1, std::string version2) {
  auto v1 = split(version1, '.');
  auto v2 = split(version2, '.');
  unsigned int max = std::max(v1.size(), v2.size());

  while (v1.size() < max) v1.push_back("0");
  while (v2.size() < max) v2.push_back("0");

  for (unsigned int i = 0; i < max; i++) {
    try {
      unsigned int n1 = std::stoi(v1[i].empty() ? "0" : v1[i]);
      unsigned int n2 = std::stoi(v2[i].empty() ? "0" : v2[i]);
      if (n1 > n2) return 1;
      if (n1 < n2) return 2;
    } catch (const std::invalid_argument &) {
      // fallback default
      return 0;
    }
  }
  return 0;
}

static QString GetIniFilename() {
#ifdef Q_OS_MACOS
  QFileInfo applicationPath = qApp->applicationFilePath();
  //  qDebug() << QString(applicationPath.absolutePath());
  // on macOS excecutable file is located in
  // ./rclone-browser.app/Contents/MasOS/ to get actual bundle folder we have to
  // traverse three levels up
  QFileInfo MacOSPath = applicationPath.dir().path();
  QFileInfo ContentsPath = MacOSPath.dir().path();
  QFileInfo appBundlePath = ContentsPath.dir().path();
  //  qDebug() << QString("utils.cpp appBundle.absolutePath: " +
  //                      appBundlePath.absolutePath());
  //  qDebug() << QString(
  //      "utils.cpp ini file:" +
  //      appBundlePath.dir().filePath(appBundlePath.baseName() + ".ini"));
  return appBundlePath.dir().filePath(appBundlePath.baseName() + ".ini");
#else
#ifdef Q_OS_WIN
  QFileInfo applicationPath(qApp->applicationFilePath());
  return applicationPath.dir().filePath(applicationPath.baseName() + ".ini");
#else
  QString xdg_config_home = qgetenv("XDG_CONFIG_HOME");
  return xdg_config_home + "/rclone-browser/rclone-browser.ini";
#endif
#endif
}

bool IsPortableMode() {
  QString ini = GetIniFilename();
  QString xdg_config_home = qgetenv("XDG_CONFIG_HOME");
  //  qDebug() << QString("utils.cpp $XDG_CONFIG_HOME: " + xdg_config_home);
  QString appimage = qgetenv("APPIMAGE");
  //  qDebug() << QString("utils.cpp $APPIMAGE: " + appimage);

  // cat ".config" from $XDG_CONFIG_HOME
  // it should be the same as appimage if run from AppImage
  xdg_config_home = xdg_config_home.left(xdg_config_home.length() - 7);
  //  qDebug() << QString("utils.cpp $XDG_CONFIG_HOME-7: " + xdg_config_home);

  if (!xdg_config_home.isEmpty() && !appimage.isEmpty() &&
      xdg_config_home == appimage) {

    return true;
  }

  if (QFileInfo(ini).exists(ini)) {

    return true;
  } else {
    return false;
  }

  //  return QFileInfo(ini).exists();
}

std::unique_ptr<QSettings> GetSettings() {
  if (IsPortableMode()) {
    return std::unique_ptr<QSettings>(
        new QSettings(GetIniFilename(), QSettings::IniFormat));
  }
  return std::unique_ptr<QSettings>(new QSettings);
}

void ReadSettings(QSettings *settings, QObject *widget) {
  QString name = widget->objectName();

  if (!name.isEmpty() &&
      (settings->contains(name) || settings->contains(name + "/size"))) {

    if (QRadioButton *obj = qobject_cast<QRadioButton *>(widget)) {
      obj->setChecked(settings->value(name).toBool());
      return;
    }
    if (QCheckBox *obj = qobject_cast<QCheckBox *>(widget)) {
      obj->setChecked(settings->value(name).toBool());
      return;
    }
    if (QComboBox *obj = qobject_cast<QComboBox *>(widget)) {
      obj->setCurrentIndex(settings->value(name).toInt());
      return;
    }
    if (QSpinBox *obj = qobject_cast<QSpinBox *>(widget)) {
      obj->setValue(settings->value(name).toInt());
      return;
    }
    if (QLineEdit *obj = qobject_cast<QLineEdit *>(widget)) {
      obj->setText(settings->value(name).toString());
      return;
    }

    if (QPlainTextEdit *obj = qobject_cast<QPlainTextEdit *>(widget)) {
      int count = settings->beginReadArray(name);
      QStringList lines;
      lines.reserve(count);
      for (int i = 0; i < count; i++) {
        settings->setArrayIndex(i);
        lines.append(settings->value("value").toString());
      }
      settings->endArray();

      obj->setPlainText(lines.join('\n'));
      return;
    }
  }

  for (auto child : widget->children()) {
    ReadSettings(settings, child);
  }
}

void WriteSettings(QSettings *settings, QObject *widget) {
  QString name = widget->objectName();

  if (QRadioButton *obj = qobject_cast<QRadioButton *>(widget)) {
    settings->setValue(name, obj->isChecked());
    return;
  }
  if (QCheckBox *obj = qobject_cast<QCheckBox *>(widget)) {
    settings->setValue(name, obj->isChecked());
    return;
  }
  if (QComboBox *obj = qobject_cast<QComboBox *>(widget)) {
    settings->setValue(name, obj->currentIndex());
    return;
  }
  if (QSpinBox *obj = qobject_cast<QSpinBox *>(widget)) {
    settings->setValue(name, obj->value());
    return;
  }
  if (QLineEdit *obj = qobject_cast<QLineEdit *>(widget)) {
    if (obj->text().isEmpty()) {
      settings->remove(name);
    } else {
      settings->setValue(name, obj->text());
    }
    return;
  }
  if (QPlainTextEdit *obj = qobject_cast<QPlainTextEdit *>(widget)) {

    QString text = obj->toPlainText().trimmed();
    if (!text.isEmpty()) {
      QStringList lines = text.split('\n');
      settings->beginWriteArray(name, lines.size());
      for (int i = 0; i < lines.count(); i++) {
        settings->setArrayIndex(i);
        settings->setValue("value", lines[i]);
      }
      settings->endArray();
    } else {
      settings->beginWriteArray(name, 0);
      settings->endArray();
    }
    return;
  }

  for (auto child : widget->children()) {
    WriteSettings(settings, child);
  }
}

QStringList GetRcloneConf() {
  if (gRcloneConf.isEmpty()) {
    return QStringList();
  }

  QString conf = gRcloneConf;
  if (IsPortableMode() && QFileInfo(conf).isRelative()) {
#ifdef Q_OS_MACOS
    // on macOS excecutable file is located in
    // ./rclone-browser.app/Contents/MasOS/rclone-browser to get actual bundle
    // folder we have to traverse three levels up
    conf = QDir(qApp->applicationDirPath() + "/../../..").filePath(conf);
#else
#ifdef Q_OS_WIN
    conf = QDir(qApp->applicationDirPath()).filePath(conf);
#else
    QString xdg_config_home = qgetenv("XDG_CONFIG_HOME");
    conf = QDir(xdg_config_home + "/..").filePath(conf);
#endif
#endif
    //    qDebug() << QString("utils.cpp conf: " + conf);
  }
  return QStringList() << "--config" << conf;
}

void SetRcloneConf(const QString &rcloneConf) { gRcloneConf = rcloneConf; }

QString GetRclone() {
  QString rclone = gRclone;
  if (IsPortableMode() && QFileInfo(rclone).isRelative()) {
#ifdef Q_OS_MACOS
    // on macOS excecutable file is located in
    // ./rclone-browser.app/Contents/MasOS/rclone-browser to get actual bundle
    // folder we have to traverse three levels up
    rclone = QDir(qApp->applicationDirPath() + "/../../..").filePath(rclone);
#else
#ifdef Q_OS_WIN
    rclone = QDir(qApp->applicationDirPath()).filePath(rclone);
#else
    QString xdg_config_home = qgetenv("XDG_CONFIG_HOME");
    rclone = QDir(xdg_config_home + "/..").filePath(rclone);
#endif
#endif
    //    qDebug() << QString("utils.cpp rclone portable: " + rclone);
  }

  return rclone;
}

void SetRclone(const QString &rclone) { gRclone = rclone.trimmed(); }

void UseRclonePassword(QProcess *process) {
  const QString proxyUrl = localHostsProxyUrl();

  if (!gRclonePassword.isEmpty() || !proxyUrl.isEmpty()) {
    QProcessEnvironment env = process->processEnvironment();
    if (env.isEmpty()) {
      env = QProcessEnvironment::systemEnvironment();
    }

    if (!proxyUrl.isEmpty()) {
      env.insert("HTTPS_PROXY", proxyUrl);
      env.insert("https_proxy", proxyUrl);
    }

    if (!gRclonePassword.isEmpty()) {
      env.insert("RCLONE_CONFIG_PASS", gRclonePassword);
    }

    process->setProcessEnvironment(env);
  }
}

void SetRclonePassword(const QString &rclonePassword) {
  gRclonePassword = rclonePassword;
}

QStringList GetRemoteModeRcloneOptions() {
  auto settings = GetSettings();
  QString googleDriveMode =
      settings->value("Settings/remoteMode", "main").toString();

  QStringList driveSharedOption;

  if (googleDriveMode == "shared") {
    driveSharedOption << "--drive-shared-with-me";
  }
  if (googleDriveMode == "trash") {
    driveSharedOption << "--drive-trashed-only";
  }
  return driveSharedOption;
}

QStringList GetDefaultOptionsList(const QString &settingsOptions) {
  auto settings = GetSettings();
  QString defaultOptions =
      settings->value("Settings/" + settingsOptions).toString();
  //      settings->value("Settings/defaultRcloneOptions").toString();
  QStringList defaultOptionsList;

  if (!defaultOptions.isEmpty()) {
    QRegularExpression re(R"( (?=[^"]*("[^"]*"[^"]*)*$))");

    for (QString arg : defaultOptions.split(re)) {
      if (!arg.isEmpty()) {
        defaultOptionsList << arg.replace("\"", "");
      }
    }
  }

  return defaultOptionsList;
}

QStringList GetShowHidden() {
  auto settings = GetSettings();
  bool showHidden = settings->value("Settings/showHidden", true).toBool();
  QStringList showHiddenOption;
  if (!showHidden) {
    showHiddenOption << "--exclude"
                     << ".*/**"
                     << "--exclude"
                     << ".*";
  }
  return showHiddenOption;
}

QDir GetConfigDir() {

  QDir outputDir;

  if (IsPortableMode()) {
    // in portable mode tasks' file will be saved in the same folder as
    // excecutable
#ifdef Q_OS_MACOS
    // on macOS excecutable file is located in
    // ./rclone-browser.app/Contents/MasOS/
    // to get actual bundle folder we have
    // to traverse three levels up
    outputDir = QDir(qApp->applicationDirPath() + "/../../..");
#else
#ifdef Q_OS_WIN
    // not macOS
    outputDir = QDir(qApp->applicationDirPath());
#else
    QString xdg_config_home = qgetenv("XDG_CONFIG_HOME");
    outputDir = QDir(xdg_config_home + "/rclone-browser");
#endif
#endif

  } else {
    // get data location folder from Qt  - OS dependend
    outputDir =
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
  }

  // if (!outputDir.exists()) {
  //   outputDir.mkpath(".");
  // }
  // QString filePath = outputDir.absoluteFilePath(persistenceFileName);
  // QFile *file = new QFile(filePath);

  // if (!file->open(mode)) {
  //   qDebug() << QString("Could not open ") << file->fileName();
  //   delete file;
  //   file = nullptr;
  // }
  return outputDir;
}

// build rclone cmd string (used for info, e.g. to show rclone cmd in transfer
// dialog)
QStringList GetRcloneCmd(const QStringList &args) {

  QStringList rcloneTransferCmd;

  QString rcloneCmd = (QDir::toNativeSeparators(GetRclone()));
  QStringList rcloneConf = GetRcloneConf();

  QStringList rcloneOptions = args;

  // rclone executable
  if (!rcloneCmd.isEmpty()) {
    if (rcloneCmd.contains(" ")) {
      rcloneTransferCmd << "\"" + rcloneCmd + "\"";
    } else {
      rcloneTransferCmd << rcloneCmd;
    }
  }

  // rclone config
  if (!rcloneConf.isEmpty()) {
    // --config
    rcloneTransferCmd << rcloneConf.at(0);
    // file location
    if (rcloneConf.at(1).contains(" ")) {
      rcloneTransferCmd << "\"" + QDir::toNativeSeparators(rcloneConf.at(1)) +
                               "\"";
    } else {
      rcloneTransferCmd << QDir::toNativeSeparators(rcloneConf.at(1));
    }
  }

  if (!rcloneOptions.isEmpty() && rcloneOptions.count() > 1) {
    // copy/move/sync
    rcloneTransferCmd << rcloneOptions.takeAt(0);

    // source and destination
    if (rcloneOptions.at(0).contains(" ")) {
      rcloneTransferCmd << "\"" + rcloneOptions.takeAt(0) + "\"";
    } else {
      rcloneTransferCmd << rcloneOptions.takeAt(0);
    }

    if (rcloneOptions.at(0).contains(" ")) {
      rcloneTransferCmd << "\"" + rcloneOptions.takeAt(0) + "\"";
    } else {
      rcloneTransferCmd << rcloneOptions.takeAt(0);
    }
  }

  // rclone remaining options
  for (int j = 0; j < rcloneOptions.count(); ++j) {
    if (rcloneOptions.at(j).contains(" ")) {
      rcloneTransferCmd << "\"" + rcloneOptions.at(j) + "\"";
    } else {
      rcloneTransferCmd << rcloneOptions.at(j);
    }
  }

  return rcloneTransferCmd;
}
