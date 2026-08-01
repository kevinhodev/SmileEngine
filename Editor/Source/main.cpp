#include "SmileEditor/DarkTheme.h"
#include "SmileEditor/MainWindow.h"
#include "Smile/Core/Logger.h"
#include "Smile/Core/VersionInfo.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QSysInfo>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>

namespace {
    constexpr int kRetainedLogSessions = 10;

    void LogQString(Smile::LogLevel _Level, const QString& _Text) {
        const QByteArray Utf8 = _Text.toUtf8();
        Smile::Log(_Level, std::string_view(
            Utf8.constData(), static_cast<size_t>(Utf8.size())));
    }

    QString MakeSessionLogPath() {
        QString DataRoot =
            QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (DataRoot.isEmpty()) DataRoot = QCoreApplication::applicationDirPath();

        QDir DataDir(DataRoot);
        if (!DataDir.mkpath(QStringLiteral("Logs"))) return {};

        const QString Stamp = QDateTime::currentDateTimeUtc().toString(
            QStringLiteral("yyyyMMdd'T'HHmmss.zzz'Z'"));
        const QString FileName = QStringLiteral("SmileEditor-%1-p%2.log")
            .arg(Stamp)
            .arg(QCoreApplication::applicationPid());
        return DataDir.filePath(QStringLiteral("Logs/") + FileName);
    }

    void PruneSessionLogs(const QString& _CurrentLogPath) {
        const QFileInfo Current(_CurrentLogPath);
        QDir LogDir(Current.absolutePath());
        const QFileInfoList Logs = LogDir.entryInfoList(
            QStringList{ QStringLiteral("SmileEditor-*.log") },
            QDir::Files, QDir::Time);

        for (qsizetype I = kRetainedLogSessions; I < Logs.size(); ++I)
            QFile::remove(Logs.at(I).absoluteFilePath());
    }

    // Registra as fontes empacotadas (Fonts/ ao lado do exe) antes de qualquer UI subir. Vale
    // p/ os dois mundos: QSS dos widgets e Theme.fontFamily do QML. Se falhar, o Qt cai na
    // familia seguinte sozinho — a UI continua legivel, so sem a Inter.
    void RegisterBundledFonts() {
        const QString Dir = QCoreApplication::applicationDirPath() + QStringLiteral("/Fonts");
        const QStringList Files = QDir(Dir).entryList(
            QStringList{ QStringLiteral("*.ttf"), QStringLiteral("*.otf") }, QDir::Files);

        for (const QString& F : Files) {
            const QString Path = Dir + QLatin1Char('/') + F;
            const int Id = QFontDatabase::addApplicationFont(Path);
            if (Id < 0) {
                LogQString(Smile::LogLevel::Warning,
                           QStringLiteral("Falha ao registrar a fonte %1").arg(F));
                continue;
            }
            // Loga a familia REAL que o Qt registrou: em fonte variavel o nome nem sempre e o
            // do arquivo, e o Theme precisa bater exatamente com ela.
            LogQString(Smile::LogLevel::Debug,
                       QStringLiteral("Fonte registrada: %1 -> %2")
                           .arg(F, QFontDatabase::applicationFontFamilies(Id).join(
                                       QStringLiteral(", "))));
        }
    }

    void InitializeSessionLogging() {
        const QString LogPath = MakeSessionLogPath();
        if (LogPath.isEmpty()) {
            Smile::LogWarning("Nao foi possivel criar o diretorio de logs da sessao");
            return;
        }

        Smile::LogConfig Config;
        Config.FilePath = std::filesystem::path(LogPath.toStdWString());
        if (!Smile::InitializeLogger(Config)) {
            LogQString(Smile::LogLevel::Warning,
                       QStringLiteral("Nao foi possivel abrir o log persistente: %1").arg(LogPath));
            return;
        }
        PruneSessionLogs(LogPath);

#if defined(_DEBUG)
        const QString BuildConfig = QStringLiteral("Debug");
#else
        const QString BuildConfig = QStringLiteral("Release");
#endif

        LogQString(Smile::LogLevel::Debug,
            QStringLiteral("SmileEditor %1 | build %2 | %3 | Qt %4")
                .arg(QStringLiteral(SMILE_VERSION_STRING),
                     QStringLiteral(SMILE_BUILD_NUMBER),
                     BuildConfig,
                     QString::fromLatin1(qVersion())));
        LogQString(Smile::LogLevel::Debug,
            QStringLiteral("Sessao: pid=%1 | OS=%2 | arch=%3 | cwd=%4")
                .arg(QCoreApplication::applicationPid())
                .arg(QSysInfo::prettyProductName(),
                     QSysInfo::currentCpuArchitecture(),
                     QDir::currentPath()));
        LogQString(Smile::LogLevel::Debug,
            QStringLiteral("Arquivo de log: %1").arg(QDir::toNativeSeparators(LogPath)));
    }

    int RunEditor(int _Argc, char* _Argv[]) {
        QApplication App(_Argc, _Argv);
        QApplication::setApplicationName(QStringLiteral("SmileEditor"));
        QApplication::setOrganizationName(QStringLiteral("SmileEngine"));

        // Estilo Basic dos QtQuick.Controls: 100% customizavel, sem decoracoes nativas (setas,
        // groove, focus frame) que vazavam por baixo da ThinScrollBar. Deve vir antes de QML.
        QQuickStyle::setStyle(QStringLiteral("Basic"));

        InitializeSessionLogging();
        RegisterBundledFonts();
        SmileEditor::ApplyDarkTheme(App);

        int ExitCode = EXIT_FAILURE;
        {
            SmileEditor::MainWindow Window;
            // Dev/smoke: caminho de .sscene como 1o argumento carrega a cena no boot.
            const QStringList Args = QApplication::arguments();
            if (Args.size() > 1 &&
                Args.at(1).endsWith(QStringLiteral(".sscene"), Qt::CaseInsensitive)) {
                Window.SetStartupScene(Args.at(1));
            }
            Window.show();

            ExitCode = App.exec();

            // Garante que nenhum produtor alcance o model enquanto a arvore Qt e destruida.
            Smile::SetLogSink({});
        }

        Smile::LogDebug("Sessao encerrada normalmente");
        return ExitCode;
    }
}

int main(int argc, char* argv[]) {
    int ExitCode = EXIT_FAILURE;
    try {
        ExitCode = RunEditor(argc, argv);
    } catch (const std::exception& Error) {
        // O construtor da MainWindow pode ter instalado o sink antes de falhar.
        Smile::SetLogSink({});
        Smile::LogError(std::string("Excecao fatal no SmileEditor: ") + Error.what());
    } catch (...) {
        Smile::SetLogSink({});
        Smile::LogError("Excecao fatal desconhecida no SmileEditor");
    }

    Smile::FlushLogs();
    Smile::ShutdownLogger();
    return ExitCode;
}
