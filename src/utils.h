#pragma once

std::unique_ptr<QSettings> GetSettings();

void ReadSettings(QSettings *settings, QObject *widget);
void WriteSettings(QSettings *settings, QObject *widget);

bool IsPortableMode();

QString GetRclone();
void SetRclone(const QString &rclone);

QStringList GetRcloneConf();
void SetRcloneConf(const QString &rcloneConf);

void UseRclonePassword(QProcess *process);
void SetRclonePassword(const QString &rclonePassword);

QStringList GetDefaultOptionsList(const QString &settingsOptions);
QStringList GetRemoteModeRcloneOptions();
QStringList GetShowHidden();
QStringList GetRcloneCmd(const QStringList &args);

QDir GetConfigDir(void);

QString GetOpenFileNameNative(QWidget *parent, const QString &caption,
                              const QString &dir = QString(),
                              const QString &filter = QString());
QStringList GetOpenFileNamesNative(QWidget *parent, const QString &caption,
                                   const QString &dir = QString(),
                                   const QString &filter = QString());
QString GetSaveFileNameNative(QWidget *parent, const QString &caption,
                              const QString &dir = QString(),
                              const QString &filter = QString());
QString GetExistingDirectoryNative(QWidget *parent, const QString &caption,
                                   const QString &dir = QString());

unsigned int compareVersion(std::string, std::string);
