#include "mainwindow.h"
#include <ckbnextconfig.h>
#include <QApplication>
#include <QDateTime>
#include <QSharedMemory>
#include <QCommandLineParser>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <QTranslator>
#include "compat/qrand.h"
#include <QMessageBox>
#include "keywidgetdebugger.h"
#include <QSurfaceFormat>
#include <iostream>
#include "ckbsystemquirks.h"

QSharedMemory appShare("ckb-next");

#ifdef Q_OS_MACOS
// App Nap is an OSX feature designed to save power by putting background apps to sleep.
// However, it interferes with ckb's animations, so we want it off
extern "C" void disableAppNap();
#endif

// Shared memory sizes
#define SHMEM_SIZE      128 * 1024
#define SHMEM_SIZE_V015 16

// Command line options
enum CommandLineParseResults {
    CommandLineOK,
    CommandLineError,
    CommandLineVersionRequested,
    CommandLineHelpRequested,
    CommandLineClose,
    CommandLineBackground,
    CommandLineSwitchToProfile,
    CommandLineSwitchToMode,
    CommandLineSleep,
};

bool startDelay = false;
bool silent = false;
#ifndef QT_NO_DEBUG
bool kwdebug = false;
#endif

/**
 * parseCommandLine - Setup options and parse command line arguments.
 *
 * @param parser        parser instance to use for parse the arguments
 * @param errorMessage  argument parse error message
 *
 * @return  integer, representing the requested argument
 */
CommandLineParseResults parseCommandLine(QCommandLineParser &parser, QString *errorMessage) {
    // setup parser to interpret -abc as -a -b -c
    parser.setSingleDashWordOptionMode(QCommandLineParser::ParseAsCompactedShortOptions);

    /* add command line options */
    // add -v, --version
    const QCommandLineOption versionOption = parser.addVersionOption();
    // add -h, --help (/? on windows)
    const QCommandLineOption helpOption = parser.addHelpOption();
    // add -b, --background
    const QCommandLineOption backgroundOption(QStringList() << "b" << "background",
                                              QObject::tr("Starts in background, without displaying the main window."));
    parser.addOption(backgroundOption);

    // add -c, --close
    const QCommandLineOption closeOption(QStringList() << "c" << "close",
                                         QObject::tr("Causes already running instance (if any) to exit."));
    parser.addOption(closeOption);

    const QCommandLineOption switchToProfileOption(QStringList() << "p" << "profile", QObject::tr("Switches to the profile with the specified name on all devices, or only the device given by --device."), "profile-name");
    parser.addOption(switchToProfileOption);

    const QCommandLineOption switchToProfileSelectOption(QStringList() << "P" << "profile-select",
                                         QObject::tr("Switches to the profile at the given relative or absolute position (next/prev/first/last, case-insensitive, or a positive 1-based index) on all devices, or only the device given by --device. Cannot be combined with --profile/-p."), "profile-selector");
    parser.addOption(switchToProfileSelectOption);

    const QCommandLineOption switchToModeOption(QStringList() << "m" << "mode", QObject::tr("Switches to the mode either in the current profile, or in the one specified by --profile or --profile-select, on all devices, or only the device given by --device."), "mode-name");
    parser.addOption(switchToModeOption);

    const QCommandLineOption switchToModeSelectOption(QStringList() << "M" << "mode-select",
                                         QObject::tr("Switches to the mode at the given relative or absolute position (next/prev/first/last, case-insensitive, or a positive 1-based index) within the current/target profile, on all devices, or only the device given by --device. Cannot be combined with --mode/-m."), "mode-selector");
    parser.addOption(switchToModeSelectOption);

    const QCommandLineOption deviceOption(QStringList() << "D" << "device",
                                         QObject::tr("Restricts --profile/--mode/--profile-select/--mode-select switching to the device with the given USB serial."), "device-serial");
    parser.addOption(deviceOption);

    const QCommandLineOption sleepOption(QStringList() << "z" << "sleep", QObject::tr("Turns the lights off as if the system was idling."));
    parser.addOption(sleepOption);

    QCommandLineOption delayOption(QStringList() << "d" << "delay", QObject::tr("Delays application start for 5 seconds"));
    QCommandLineOption silentOption(QStringList() << "s" << "silent", QObject::tr("Disables the daemon not running popup"));

    // Sigh
#if QT_VERSION >= QT_VERSION_CHECK(5, 8, 0)
    delayOption.setFlags(QCommandLineOption::HiddenFromHelp);
    silentOption.setFlags(QCommandLineOption::HiddenFromHelp);
#elif QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)
    delayOption.setHidden(true);
    silentOption.setHidden(true);
#endif
    parser.addOption(delayOption);
    parser.addOption(silentOption);

#ifndef QT_NO_DEBUG
    QCommandLineOption kwdebugOption("kwdebug", QObject::tr("Enables the KeyWidget debug window"));
    parser.addOption(kwdebugOption);
#endif

    /* parse arguments */
    if (!parser.parse(QCoreApplication::arguments())) {
        // set error, if there already are some
        *errorMessage = parser.errorText();
        return CommandLineError;
    }

    /* return requested operation/setup */
    if (parser.isSet(versionOption)) {
        // print version and exit
        return CommandLineVersionRequested;
    }

    if (parser.isSet(helpOption)) {
        // print help and exit
        return CommandLineHelpRequested;
    }

    if(parser.isSet(delayOption)) {
        startDelay = true;
    }

    if(parser.isSet(silentOption)) {
        silent = true;
    }

#ifndef QT_NO_DEBUG
    if(parser.isSet(kwdebugOption)) {
        kwdebug = true;
    }
#endif

    if (parser.isSet(backgroundOption)) {
        // open application in background
        return CommandLineBackground;
    }

    if (parser.isSet(closeOption)) {
        // close already running application instances, if any
        return CommandLineClose;
    }

    if(parser.isSet(switchToProfileOption)) {
        return CommandLineSwitchToProfile;
    }

    if(parser.isSet(switchToProfileSelectOption)) {
        return CommandLineSwitchToProfile;
    }

    if(parser.isSet(switchToModeOption)) {
        return CommandLineSwitchToMode;
    }

    if(parser.isSet(switchToModeSelectOption)) {
        return CommandLineSwitchToMode;
    }

    if(parser.isSet(sleepOption)) {
        return CommandLineSleep;
    }

    // --device given without --profile/--mode/--profile-select/--mode-select: route into the
    // same validation block as those four (they share one case body below), which reports the
    // "-D needs -p/-m/-P/-M" error. Reusing CommandLineSwitchToProfile here is cosmetic - which
    // of the two shared enum values is returned doesn't matter since both hit the same case.
    if(parser.isSet(deviceOption)) {
        return CommandLineSwitchToProfile;
    }

    /* no explicit argument was passed */
    return CommandLineOK;
}

// Scan shared memory for an active PID
static bool pidActive(const QStringList& lines){
    foreach(const QString& line, lines){
        if(line.startsWith("PID ")){
            bool ok;
            pid_t pid;
            // Valid PID found?
            if((pid = line.split(" ")[1].toLong(&ok)) > 0 && ok){
                // kill -0 does nothing to the application, but checks if it's running
                return (kill(pid, 0) == 0 || errno != ESRCH);
            }
        }
    }
    // If the PID wasn't listed in the shmem, assume it is running
    return true;
}

// Check if the application is running. Optionally write something to its shared memory.
static bool isRunning(const char* command){
    // Try to create shared memory (if successful, application was not already running)
    // We try to create it twice, because if ckb-next crashes a certain way (ASAN), then we end up
    // with a situation where create() fails with QSharedMemory::AlreadyExists
    // but then attach() also fails with QSharedMemory::NotFound.
    // By trying to create it twice, if it fails twice, that means it actually already exists.
    if((!appShare.create(SHMEM_SIZE) && !appShare.create(SHMEM_SIZE)) || !appShare.lock()){
        // Lock existing shared memory
        if(!appShare.attach() || !appShare.lock())
            return true;
        bool running = false;
        if(appShare.size() == SHMEM_SIZE_V015){
            // Old shmem - no PID listed so assume the process is running, and print the command directly to the buffer
            if(command){
                void* data = appShare.data();
                snprintf((char*)data, SHMEM_SIZE_V015, "%s", command);
            }
            running = true;
        } else {
            // New shmem. Scan the contents of the shared memory for a PID
            QStringList lines = QString((const char*)appShare.constData()).split("\n");
            if(pidActive(lines)){
                running = true;
                // Append the command
                if(command){
                    lines.append(QString("Option ") + command);
                    QByteArray newMem = lines.join("\n").left(SHMEM_SIZE).toLatin1();
                    // Copy the NUL byte as well as the string
                    memcpy(appShare.data(), newMem.constData(), newMem.length() + 1);
                }
            }
        }
        if(running){
            appShare.unlock();
            return true;
        }
    }
    // Not already running. Initialize the shared memory with our PID
    snprintf((char*)appShare.data(), appShare.size(), "PID %ld", (long)getpid());
    appShare.unlock();
    return false;
}

bool checkIfQtCreator(){
#ifdef Q_OS_LINUX
    QString file = QString("/proc/%1/exe").arg(QString::number((long)getppid()));

    QFileInfo f(file);
    if(!f.exists())
        return false;

    QString exepath = f.canonicalFilePath();
    if(exepath.endsWith("/qtcreator"))
        return true;
#endif
    return false;
}

const char* DISPLAY = nullptr;
const char* argv0 = nullptr;

int main(int argc, char* argv[]){
    // Warning: The order of everything in main() is very fragile
    // Please be very careful if shuffling things around

    // First assign argv0 because we need it in CkbSystemQuirks
    if(argc > 0)
        argv0 = argv[0];

    // Setup names and versions
    // This needs to be done before the first QSettings is created
    QCoreApplication::setOrganizationName("ckb-next");
    QCoreApplication::setApplicationVersion(CKB_NEXT_VERSION_STR);
    QCoreApplication::setApplicationName("ckb-next");

    // Initialize a temporary QSettings very early to apply quirks before QApplication is created
    QSettings::setDefaultFormat(CkbSettings::Format);
    QSettings* tmpSettings = new QSettings();

    // Apply OpenGL-related settings before QApplication
    // Note: The settings are not exposed in the UI
    QSurfaceFormat fmt;

    int msaa = tmpSettings->value("Program/GL/MSAA", CkbSystemQuirks::getMaxMSAA()).toInt();
    if(msaa >= 0 && msaa <= 16)
        fmt.setSamples(msaa);

    // HACK: Disable vsync so that the GUI thread isn't blocked when monitors enter power saving
    const int swapInterval = tmpSettings->value("Program/GL/SwapInterval", 0).toInt();
    if(swapInterval >= 0)
        fmt.setSwapInterval(swapInterval);

    QSurfaceFormat::setDefaultFormat(fmt);

#ifdef Q_OS_LINUX
    // Get rid of "-session" before Qt parses the arguments
    // Also store any value of -display
    for(int i = 1; i < argc; i++){
        QByteArray arg(argv[i]);
        if (arg.startsWith("--"))
            arg.remove(0, 1);
        if(arg == "-session"){
            argv[i][1] = 'b';
            argv[i][2] = '\0';
            if(i + 1 < argc) {
                argv[++i][0] = '\0';
            }
        } else if (arg == "-display" && i < argc - 1) {
            DISPLAY = argv[++i];
        }
    }
#endif

#if QT_VERSION >= QT_VERSION_CHECK(5, 6, 0) && QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    // Explicitly request high dpi scaling if desired
    // Needs to be called before QApplication is constructed
    if(tmpSettings->value("Program/HiDPI", false).toBool())
        QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif

    // Setup main application
    QApplication a(argc, argv);

    // Setup translations
    QTranslator translator, qttranslator;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#define LOCATION location
#else
#define LOCATION path
#endif
    if(qttranslator.load(QLocale(), QStringLiteral("qt"), QStringLiteral("_"), QLibraryInfo::LOCATION(QLibraryInfo::TranslationsPath)))
        a.installTranslator(&qttranslator);
#undef LOCATION

    if(translator.load(QLocale(), QStringLiteral(""), QStringLiteral(""), QStringLiteral(":/translations")))
        a.installTranslator(&translator);

    const quint16 currentSettingsVersion = tmpSettings->value("Program/SettingsVersion", 0).toInt();
    if(currentSettingsVersion && currentSettingsVersion > CKB_NEXT_SETTINGS_VER){
        if(QMessageBox::warning(nullptr, QObject::tr("Downgrade Warning"),
                                QObject::tr("Downgrading ckb-next will lead to profile data loss. "
                                            "It is recommended to click Cancel and update to the latest version.<br><br>"
                                            "If you wish to continue, back up the settings file located at<blockquote>%1</blockquote>and click OK.").arg(tmpSettings->fileName()),
                                QMessageBox::Ok, QMessageBox::Cancel)
                == QMessageBox::Cancel){
            delete tmpSettings;
            tmpSettings = nullptr;
            return 0;
        }
        // Only handle the downgrade here. The upgrade is handled in CkbSettings.
        tmpSettings->setValue("Program/SettingsVersion", CKB_NEXT_SETTINGS_VER);
    }

    // Setup argument parser
    QCommandLineParser parser;
    QString errorMessage;
    parser.setApplicationDescription(CKB_NEXT_DESCRIPTION);
    bool background = false;

    // Although the daemon runs as root, the GUI needn't and shouldn't be, as it has the potential to corrupt settings data.
    if(getuid() == 0){
        printf("The ckb-next GUI should not be run as root.\n");
        return 0;
    }

    // Seed the RNG for UsbIds
    Q_SRAND(QDateTime::currentMSecsSinceEpoch());

#if QT_VERSION >= QT_VERSION_CHECK(5, 3, 0) && QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    // Enable HiDPI support
    qApp->setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif

    // Parse arguments
    switch (parseCommandLine(parser, &errorMessage)) {
    case CommandLineOK:
        // If launched with no argument
        break;
    case CommandLineError:
        fputs(qPrintable(errorMessage), stderr);
        fputs("\n\n", stderr);
        fputs(qPrintable(parser.helpText()), stderr);
        return 1;
    case CommandLineVersionRequested:
        // If launched with --version, print version info and then quit
        printf("%s %s\n", qPrintable(QCoreApplication::applicationName()),
               qPrintable(QCoreApplication::applicationVersion()));
        return 0;
    case CommandLineHelpRequested:
        // If launched with --help, print help and then quit
        parser.showHelp();
        return 0;
    case CommandLineClose:
        // If launched with --close, kill existing app
        if (isRunning("Close"))
            printf("Asking existing instance to close.\n");
        else
            printf("ckb-next is not running.\n");
        return 0;
    case CommandLineSwitchToMode:
    case CommandLineSwitchToProfile:
    {
        if(parser.isSet("profile") && parser.isSet("profile-select")) {
            fprintf(stderr, "Error: --profile/-p and --profile-select/-P cannot be used together.\n");
            return 1;
        }
        if(parser.isSet("mode") && parser.isSet("mode-select")) {
            fprintf(stderr, "Error: --mode/-m and --mode-select/-M cannot be used together.\n");
            return 1;
        }

        QString profileName = parser.value("profile").left(30);
        QString modeName = parser.value("mode").left(30);
        QString profileSelector = parser.value("profile-select").trimmed().toLower();
        QString modeSelector = parser.value("mode-select").trimmed().toLower();
        QString deviceSerial = parser.value("device").trimmed().toUpper();

        if(parser.isSet("device") && deviceSerial.isEmpty()) {
            fprintf(stderr, "Error: --device/-D value must not be empty.\n");
            return 1;
        }

        // Grammar: "next"/"prev"/"first"/"last" (already lowercased above) or a positive
        // 1-based integer. Numeric strings are case-agnostic so toLower() above is harmless
        // to them; this mirrors -D's normalize-then-compare style (trimmed().toUpper() there).
        auto isValidSelector = [](const QString& s) -> bool {
            if(s == QLatin1String("next") || s == QLatin1String("prev")
               || s == QLatin1String("first") || s == QLatin1String("last"))
                return true;
            bool ok;
            int idx = s.toInt(&ok);
            return ok && idx >= 1;
        };

        if(parser.isSet("profile-select") && !isValidSelector(profileSelector)) {
            fprintf(stderr, "Error: --profile-select/-P value must be 'next', 'prev', 'first', 'last', or a positive integer index.\n");
            return 1;
        }
        if(parser.isSet("mode-select") && !isValidSelector(modeSelector)) {
            fprintf(stderr, "Error: --mode-select/-M value must be 'next', 'prev', 'first', 'last', or a positive integer index.\n");
            return 1;
        }

        if(profileName.isEmpty() && modeName.isEmpty() && profileSelector.isEmpty() && modeSelector.isEmpty()) {
            if(!deviceSerial.isEmpty()) {
                fprintf(stderr, "Error: --device/-D must be used together with --profile/-p, --mode/-m, --profile-select/-P, or --mode-select/-M.\n");
                return 1;
            }
            printf("No valid profile or mode specified.\n");
            return 0;
        }

        // Prefixed onto SwitchToProfile[At]:/SwitchToMode[At]: lines, duplicated per line so
        // each is self-contained (see MainWindow::timerTick()). Empty when --device wasn't
        // given, reducing this to today's exact output in that case.
        QString devicePrefix;
        if(!deviceSerial.isEmpty())
            devicePrefix = QString("Device: ").append(deviceSerial).append("\nOption ");

        QString switchStr;

        if(!profileName.isEmpty())
            switchStr.append(devicePrefix).append("SwitchToProfile: ").append(profileName).append("\nOption ");
        else if(!profileSelector.isEmpty())
            switchStr.append(devicePrefix).append("SwitchToProfileAt: ").append(profileSelector).append("\nOption ");

        if(!modeName.isEmpty())
            switchStr.append(devicePrefix).append("SwitchToMode: ").append(modeName);
        else if(!modeSelector.isEmpty())
            switchStr.append(devicePrefix).append("SwitchToModeAt: ").append(modeSelector);

        if (!isRunning(switchStr.toUtf8().constData()))
            printf("ckb-next is not running.\n");

        return 0;
    }
    case CommandLineSleep:
        if (!isRunning("Sleep"))
            printf("ckb-next is not running.\n");

        return 0;
    case CommandLineBackground:
        // If launched with --background, launch in background
        background = true;
        break;
    }

    startDelay |= tmpSettings->value("Program/StartDelay", false).toBool();
    delete tmpSettings;
    tmpSettings = nullptr;

    if(startDelay)
        QThread::sleep(5);

#ifdef Q_OS_MACOS
    disableAppNap();

    FILE *fp = fopen("/tmp/ckb", "w");
    fprintf(fp, "%d", getpid());
    fclose(fp);
#endif

    // Launch in background if requested, or if re-launching a previous session
    const char* shm_str = "Open";
    if(qApp->isSessionRestored())
    {
        background = true;
        shm_str = nullptr;
    }
    // Check if the parent was Qt Creator.
    // If so, send a close command, wait, then run.
    bool QtCreator = checkIfQtCreator();
    if(QtCreator)
        shm_str = "Close";

    if(background)
        shm_str = nullptr;

    if(isRunning(shm_str) && !QtCreator){
        printf("ckb-next is already running. Exiting.\n");
        return 0;
    }

    if(QtCreator)
        QThread::sleep(1);

    std::cout << "ckb-next " << CKB_NEXT_VERSION_STR << std::endl;
    qDebug() << "Using" << CkbSystemQuirks::getGlVendor() << CkbSystemQuirks::getGlRenderer();

    MainWindow w(silent);
    if(!background)
        w.show();

#ifndef QT_NO_DEBUG
    if(kwdebug){
        KeyWidgetDebugger* d = new KeyWidgetDebugger;
        d->show();
        QObject::connect(&w, &MainWindow::destroyed, [d](){delete d;});
    }
#endif

    return a.exec();
}
