// FDC+ side of the serial disk server protocol. Runs on its own thread and
// knows nothing about the GUI; the GUI polls view() to update its display.
#pragma once

#include "transport.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

constexpr int NUM_DRIVES = 16;

enum class StatResult { None, Ok, NoAnswer, Bad };

struct ReadResult {
    int drive = 0;
    int track = 0;
    int length = 0;             // bytes of track data requested
    int received = 0;           // bytes received, including the checksum
    bool complete = false;      // all length + 2 bytes arrived
    bool checksumGood = false;
    uint16_t expectedSum = 0;   // computed over the data received
    uint16_t receivedSum = 0;   // the checksum the server sent
    int ms = 0;
};

struct ClientView {
    bool running = false;
    bool receiving = false;     // data received within the last 3 seconds
    bool polling = false;
    std::string status;

    // STAT
    unsigned long statSent = 0;
    unsigned long statAnswered = 0;
    unsigned long statNoAnswer = 0;
    unsigned long statBad = 0;
    StatResult lastStat = StatResult::None;
    int lastStatMs = 0;
    uint16_t lastStatCode = 0;
    uint16_t mounted = 0;       // drive bitmap from the last good STAT answer

    // READ
    bool readBusy = false;
    unsigned long readCount = 0;    // completed reads; changes when result is new
    ReadResult read;

    // Log of recent exchanges, oldest first. logTotal counts every line ever
    // added, so a viewer can tell which lines are new.
    std::deque<std::string> log;
    unsigned long logTotal = 0;
};

class Client {
public:
    Client() = default;
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    void start(std::unique_ptr<Transport> transport);
    void stop();

    // Send STAT every interval while enabled.
    void setPolling(bool on);

    // Ask for a track. Ignored while another read is in progress.
    void requestRead(int drive, int track, int length);

    // Zero the STAT counters and empty the log.
    void clear();

    ClientView view() const;

    // Timing, adjustable for tests.
    std::chrono::milliseconds statInterval{250};
    std::chrono::milliseconds answerTimeout{1000};  // the FDC's own timeout

private:
    static constexpr size_t MSG_SIZE = 10;
    static constexpr size_t MAX_LOG = 200;

    void run();
    void doStat();
    void doRead(int drive, int track, int length);
    void sendCommand(const char* name, uint16_t param1, uint16_t param2);
    size_t readExact(uint8_t* buf, size_t len);
    void flushInput();
    void addLog(const std::string& line);   // caller must hold mutex_
    bool failed(int n);                     // true if the transport has failed

    mutable std::mutex mutex_;              // guards view_ and the pending read
    std::condition_variable wake_;
    ClientView view_;
    std::chrono::steady_clock::time_point lastRx_{};

    bool readPending_ = false;
    int readDrive_ = 0, readTrack_ = 0, readLength_ = 0;

    std::unique_ptr<Transport> transport_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> polling_{false};
    std::atomic<bool> pollNow_{false};
};

// Text for a drive bitmap, e.g. "Disks in drives 0, 1" or "No disks loaded".
std::string describeMounted(uint16_t mounted);
