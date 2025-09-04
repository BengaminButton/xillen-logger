#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <mutex>
#include <chrono>
#include <ctime>
#include <cstring>
#include <cstdlib>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <queue>
#include <condition_variable>

std::string author = "t.me/Bengamin_Button t.me/XillenAdapter";

enum LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARNING = 2,
    ERROR = 3,
    CRITICAL = 4
};

struct LogEntry {
    std::chrono::system_clock::time_point timestamp;
    LogLevel level;
    std::string source;
    std::string message;
    std::string threadId;
    int lineNumber;
    std::string fileName;
    
    LogEntry(LogLevel l, const std::string& s, const std::string& m, 
             const std::string& f = "", int line = 0) 
        : timestamp(std::chrono::system_clock::now()), level(l), source(s), 
          message(m), fileName(f), lineNumber(line) {
        threadId = std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    }
};

class XillenLogger {
private:
    std::string logDirectory;
    std::string logFile;
    LogLevel minLevel;
    bool consoleOutput;
    bool fileOutput;
    bool asyncMode;
    std::mutex logMutex;
    std::mutex fileMutex;
    std::queue<LogEntry> logQueue;
    std::condition_variable cv;
    std::thread workerThread;
    bool running;
    std::map<std::string, std::ofstream> fileHandles;
    size_t maxFileSize;
    int maxFiles;
    std::vector<std::string> logBuffer;
    size_t bufferSize;
    std::chrono::seconds flushInterval;
    std::chrono::system_clock::time_point lastFlush;
    
public:
    XillenLogger() : minLevel(INFO), consoleOutput(true), fileOutput(true), 
                     asyncMode(true), running(false), maxFileSize(10 * 1024 * 1024),
                     maxFiles(5), bufferSize(1000), flushInterval(std::chrono::seconds(5)) {
        logDirectory = "logs";
        logFile = "application.log";
        lastFlush = std::chrono::system_clock::now();
        initializeLogger();
    }
    
    ~XillenLogger() {
        stop();
    }
    
    void initializeLogger() {
        createLogDirectory();
        if (asyncMode) {
            startWorkerThread();
        }
    }
    
    void createLogDirectory() {
        struct stat st;
        if (stat(logDirectory.c_str(), &st) != 0) {
            if (mkdir(logDirectory.c_str(), 0755) != 0) {
                std::cerr << "Не удалось создать директорию логов: " << logDirectory << std::endl;
            }
        }
    }
    
    void startWorkerThread() {
        running = true;
        workerThread = std::thread(&XillenLogger::workerFunction, this);
    }
    
    void stop() {
        if (asyncMode && running) {
            running = false;
            cv.notify_all();
            if (workerThread.joinable()) {
                workerThread.join();
            }
        }
        flushBuffer();
        closeAllFiles();
    }
    
    void setLogLevel(LogLevel level) {
        std::lock_guard<std::mutex> lock(logMutex);
        minLevel = level;
    }
    
    void setLogFile(const std::string& filename) {
        std::lock_guard<std::mutex> lock(logMutex);
        logFile = filename;
    }
    
    void setLogDirectory(const std::string& directory) {
        std::lock_guard<std::mutex> lock(logMutex);
        logDirectory = directory;
        createLogDirectory();
    }
    
    void setConsoleOutput(bool enable) {
        std::lock_guard<std::mutex> lock(logMutex);
        consoleOutput = enable;
    }
    
    void setFileOutput(bool enable) {
        std::lock_guard<std::mutex> lock(logMutex);
        fileOutput = enable;
    }
    
    void setAsyncMode(bool enable) {
        std::lock_guard<std::mutex> lock(logMutex);
        if (asyncMode != enable) {
            asyncMode = enable;
            if (enable) {
                startWorkerThread();
            } else {
                stop();
            }
        }
    }
    
    void setMaxFileSize(size_t size) {
        std::lock_guard<std::mutex> lock(logMutex);
        maxFileSize = size;
    }
    
    void setMaxFiles(int count) {
        std::lock_guard<std::mutex> lock(logMutex);
        maxFiles = count;
    }
    
    void setBufferSize(size_t size) {
        std::lock_guard<std::mutex> lock(logMutex);
        bufferSize = size;
    }
    
    void log(LogLevel level, const std::string& source, const std::string& message,
             const std::string& fileName = "", int lineNumber = 0) {
        if (level < minLevel) {
            return;
        }
        
        LogEntry entry(level, source, message, fileName, lineNumber);
        
        if (asyncMode) {
            std::lock_guard<std::mutex> lock(logMutex);
            logQueue.push(entry);
            cv.notify_one();
        } else {
            writeLogEntry(entry);
        }
    }
    
    void debug(const std::string& source, const std::string& message,
               const std::string& fileName = "", int lineNumber = 0) {
        log(DEBUG, source, message, fileName, lineNumber);
    }
    
    void info(const std::string& source, const std::string& message,
              const std::string& fileName = "", int lineNumber = 0) {
        log(INFO, source, message, fileName, lineNumber);
    }
    
    void warning(const std::string& source, const std::string& message,
                 const std::string& fileName = "", int lineNumber = 0) {
        log(WARNING, source, message, fileName, lineNumber);
    }
    
    void error(const std::string& source, const std::string& message,
               const std::string& fileName = "", int lineNumber = 0) {
        log(ERROR, source, message, fileName, lineNumber);
    }
    
    void critical(const std::string& source, const std::string& message,
                  const std::string& fileName = "", int lineNumber = 0) {
        log(CRITICAL, source, message, fileName, lineNumber);
    }
    
private:
    void workerFunction() {
        while (running) {
            std::unique_lock<std::mutex> lock(logMutex);
            cv.wait(lock, [this] { return !logQueue.empty() || !running; });
            
            while (!logQueue.empty()) {
                LogEntry entry = logQueue.front();
                logQueue.pop();
                lock.unlock();
                
                writeLogEntry(entry);
                
                lock.lock();
            }
        }
    }
    
    void writeLogEntry(const LogEntry& entry) {
        std::string formattedMessage = formatLogEntry(entry);
        
        if (consoleOutput) {
            writeToConsole(entry, formattedMessage);
        }
        
        if (fileOutput) {
            writeToFile(entry, formattedMessage);
        }
        
        addToBuffer(formattedMessage);
    }
    
    std::string formatLogEntry(const LogEntry& entry) {
        auto time_t = std::chrono::system_clock::to_time_t(entry.timestamp);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            entry.timestamp.time_since_epoch()) % 1000;
        
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
        ss << "." << std::setfill('0') << std::setw(3) << ms.count();
        
        std::string levelStr = getLevelString(entry.level);
        std::string source = entry.source.empty() ? "MAIN" : entry.source;
        
        ss << " [" << levelStr << "] [" << source << "] [" << entry.threadId << "] ";
        
        if (!entry.fileName.empty()) {
            ss << "[" << entry.fileName;
            if (entry.lineNumber > 0) {
                ss << ":" << entry.lineNumber;
            }
            ss << "] ";
        }
        
        ss << entry.message;
        
        return ss.str();
    }
    
    std::string getLevelString(LogLevel level) {
        switch (level) {
            case DEBUG: return "DEBUG";
            case INFO: return "INFO ";
            case WARNING: return "WARN ";
            case ERROR: return "ERROR";
            case CRITICAL: return "CRIT ";
            default: return "UNKNW";
        }
    }
    
    void writeToConsole(const LogEntry& entry, const std::string& formattedMessage) {
        std::lock_guard<std::mutex> lock(logMutex);
        
        switch (entry.level) {
            case DEBUG:
                std::cout << "\033[37m" << formattedMessage << "\033[0m" << std::endl;
                break;
            case INFO:
                std::cout << "\033[32m" << formattedMessage << "\033[0m" << std::endl;
                break;
            case WARNING:
                std::cout << "\033[33m" << formattedMessage << "\033[0m" << std::endl;
                break;
            case ERROR:
                std::cout << "\033[31m" << formattedMessage << "\033[0m" << std::endl;
                break;
            case CRITICAL:
                std::cout << "\033[35m" << formattedMessage << "\033[0m" << std::endl;
                break;
        }
    }
    
    void writeToFile(const LogEntry& entry, const std::string& formattedMessage) {
        std::lock_guard<std::mutex> lock(fileMutex);
        
        std::string filename = logDirectory + "/" + logFile;
        std::ofstream& file = getFileHandle(filename);
        
        if (file.is_open()) {
            file << formattedMessage << std::endl;
            file.flush();
            
            if (shouldRotateFile(filename)) {
                rotateFile(filename);
            }
        }
    }
    
    std::ofstream& getFileHandle(const std::string& filename) {
        if (fileHandles.find(filename) == fileHandles.end()) {
            fileHandles[filename].open(filename, std::ios::app);
        }
        return fileHandles[filename];
    }
    
    bool shouldRotateFile(const std::string& filename) {
        struct stat st;
        if (stat(filename.c_str(), &st) == 0) {
            return st.st_size >= maxFileSize;
        }
        return false;
    }
    
    void rotateFile(const std::string& filename) {
        closeFile(filename);
        
        for (int i = maxFiles - 1; i > 0; i--) {
            std::string oldFile = filename + "." + std::to_string(i);
            std::string newFile = filename + "." + std::to_string(i + 1);
            
            if (access(oldFile.c_str(), F_OK) == 0) {
                rename(oldFile.c_str(), newFile.c_str());
            }
        }
        
        if (access(filename.c_str(), F_OK) == 0) {
            rename(filename.c_str(), (filename + ".1").c_str());
        }
    }
    
    void closeFile(const std::string& filename) {
        if (fileHandles.find(filename) != fileHandles.end()) {
            fileHandles[filename].close();
            fileHandles.erase(filename);
        }
    }
    
    void closeAllFiles() {
        for (auto& pair : fileHandles) {
            pair.second.close();
        }
        fileHandles.clear();
    }
    
    void addToBuffer(const std::string& message) {
        std::lock_guard<std::mutex> lock(logMutex);
        logBuffer.push_back(message);
        
        if (logBuffer.size() >= bufferSize) {
            flushBuffer();
        }
        
        auto now = std::chrono::system_clock::now();
        if (now - lastFlush >= flushInterval) {
            flushBuffer();
            lastFlush = now;
        }
    }
    
    void flushBuffer() {
        if (logBuffer.empty()) {
            return;
        }
        
        std::string bufferFilename = logDirectory + "/buffer.log";
        std::ofstream bufferFile(bufferFilename, std::ios::app);
        
        if (bufferFile.is_open()) {
            for (const auto& message : logBuffer) {
                bufferFile << message << std::endl;
            }
            bufferFile.close();
        }
        
        logBuffer.clear();
    }
    
public:
    void showStatistics() {
        std::lock_guard<std::mutex> lock(logMutex);
        
        std::cout << "\n=== СТАТИСТИКА ЛОГГЕРА ===" << std::endl;
        std::cout << "Автор: " << author << std::endl;
        std::cout << "Директория логов: " << logDirectory << std::endl;
        std::cout << "Файл логов: " << logFile << std::endl;
        std::cout << "Минимальный уровень: " << getLevelString(minLevel) << std::endl;
        std::cout << "Вывод в консоль: " << (consoleOutput ? "Включен" : "Отключен") << std::endl;
        std::cout << "Вывод в файл: " << (fileOutput ? "Включен" : "Отключен") << std::endl;
        std::cout << "Асинхронный режим: " << (asyncMode ? "Включен" : "Отключен") << std::endl;
        std::cout << "Максимальный размер файла: " << maxFileSize << " байт" << std::endl;
        std::cout << "Максимальное количество файлов: " << maxFiles << std::endl;
        std::cout << "Размер буфера: " << bufferSize << std::endl;
        std::cout << "Записей в буфере: " << logBuffer.size() << std::endl;
        std::cout << "Записей в очереди: " << logQueue.size() << std::endl;
        std::cout << "Открытых файлов: " << fileHandles.size() << std::endl;
    }
    
    void showMenu() {
        std::cout << "\n=== XILLEN LOGGER ===" << std::endl;
        std::cout << "1. Показать статистику" << std::endl;
        std::cout << "2. Изменить уровень логирования" << std::endl;
        std::cout << "3. Настройки вывода" << std::endl;
        std::cout << "4. Настройки файлов" << std::endl;
        std::cout << "5. Тестовые сообщения" << std::endl;
        std::cout << "6. Очистить логи" << std::endl;
        std::cout << "7. Показать последние записи" << std::endl;
        std::cout << "0. Выход" << std::endl;
    }
    
    void run() {
        std::cout << author << std::endl;
        std::cout << "📝 Xillen Logger запущен" << std::endl;
        
        int choice;
        while (true) {
            showMenu();
            std::cout << "Выберите опцию: ";
            std::cin >> choice;
            
            switch (choice) {
                case 1:
                    showStatistics();
                    break;
                case 2:
                    changeLogLevel();
                    break;
                case 3:
                    changeOutputSettings();
                    break;
                case 4:
                    changeFileSettings();
                    break;
                case 5:
                    sendTestMessages();
                    break;
                case 6:
                    clearLogs();
                    break;
                case 7:
                    showRecentLogs();
                    break;
                case 0:
                    std::cout << "👋 До свидания!" << std::endl;
                    return;
                default:
                    std::cout << "Неверный выбор!" << std::endl;
            }
        }
    }
    
private:
    void changeLogLevel() {
        std::cout << "Выберите уровень логирования:" << std::endl;
        std::cout << "0. DEBUG" << std::endl;
        std::cout << "1. INFO" << std::endl;
        std::cout << "2. WARNING" << std::endl;
        std::cout << "3. ERROR" << std::endl;
        std::cout << "4. CRITICAL" << std::endl;
        
        int level;
        std::cout << "Уровень: ";
        std::cin >> level;
        
        if (level >= 0 && level <= 4) {
            setLogLevel(static_cast<LogLevel>(level));
            std::cout << "✅ Уровень логирования изменен" << std::endl;
        } else {
            std::cout << "❌ Неверный уровень" << std::endl;
        }
    }
    
    void changeOutputSettings() {
        std::cout << "Включить вывод в консоль? (y/n): ";
        char choice;
        std::cin >> choice;
        setConsoleOutput(choice == 'y' || choice == 'Y');
        
        std::cout << "Включить вывод в файл? (y/n): ";
        std::cin >> choice;
        setFileOutput(choice == 'y' || choice == 'Y');
        
        std::cout << "✅ Настройки вывода изменены" << std::endl;
    }
    
    void changeFileSettings() {
        std::cout << "Максимальный размер файла (байт): ";
        size_t size;
        std::cin >> size;
        setMaxFileSize(size);
        
        std::cout << "Максимальное количество файлов: ";
        int count;
        std::cin >> count;
        setMaxFiles(count);
        
        std::cout << "✅ Настройки файлов изменены" << std::endl;
    }
    
    void sendTestMessages() {
        std::cout << "Отправка тестовых сообщений..." << std::endl;
        
        debug("TEST", "Это отладочное сообщение");
        info("TEST", "Это информационное сообщение");
        warning("TEST", "Это предупреждение");
        error("TEST", "Это ошибка");
        critical("TEST", "Это критическая ошибка");
        
        std::cout << "✅ Тестовые сообщения отправлены" << std::endl;
    }
    
    void clearLogs() {
        std::lock_guard<std::mutex> lock(fileMutex);
        
        std::string filename = logDirectory + "/" + logFile;
        std::ofstream file(filename, std::ios::trunc);
        file.close();
        
        logBuffer.clear();
        
        std::cout << "✅ Логи очищены" << std::endl;
    }
    
    void showRecentLogs() {
        std::string filename = logDirectory + "/" + logFile;
        std::ifstream file(filename);
        
        if (!file.is_open()) {
            std::cout << "❌ Не удалось открыть файл логов" << std::endl;
            return;
        }
        
        std::vector<std::string> lines;
        std::string line;
        
        while (std::getline(file, line)) {
            lines.push_back(line);
        }
        
        file.close();
        
        std::cout << "\n=== ПОСЛЕДНИЕ 10 ЗАПИСЕЙ ===" << std::endl;
        
        int start = std::max(0, static_cast<int>(lines.size()) - 10);
        for (int i = start; i < static_cast<int>(lines.size()); i++) {
            std::cout << lines[i] << std::endl;
        }
    }
};

void signalHandler(int signal) {
    std::cout << "\nПолучен сигнал " << signal << ", завершение работы..." << std::endl;
    exit(0);
}

int main() {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    XillenLogger logger;
    logger.run();
    
    return 0;
}