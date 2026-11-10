// FDC+ side of the serial disk server protocol.
//
// Commands and responses are ten byte messages: four ASCII command bytes
// followed by three little endian 16 bit words (param1, param2, checksum).
// The checksum is the 16 bit sum of the first eight bytes.
//
//   STAT  param1 LSB = selected drive (0xff = none), MSB = head status;
//         param2 = current track. The server answers STAT with response
//         code 0 and a mounted-drive bitmap in the data word.
//   READ  param1 = drive (upper nibble) and track (lower 12 bits);
//         param2 = transfer length. The server answers with the track data
//         followed by a 16 bit checksum, or not at all if the drive has no
//         disk or the track is past the end of the image.
//
// The tester only reads; it never sends WRIT, so it cannot change a disk.

#include "client.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t COMMAND_LENGTH = 8;        // does not include checksum bytes
constexpr int IDLE_WAIT_MS = 10;
constexpr auto RECEIVE_LIGHT_TIME = std::chrono::seconds(3);
constexpr uint16_t NO_DRIVE_SELECTED = 0x00ff;

uint16_t getWord(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

void putWord(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xff);
    p[1] = static_cast<uint8_t>(v >> 8);
}

uint16_t checksum(const uint8_t* p, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; ++i)
        sum += p[i];
    return static_cast<uint16_t>(sum);
}

int msSince(Clock::time_point t)
{
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count());
}

std::string hexBytes(const uint8_t* p, size_t len)
{
    std::string s;
    char buf[4];
    for (size_t i = 0; i < len; ++i) {
        std::snprintf(buf, sizeof(buf), i ? " %02X" : "%02X", p[i]);
        s += buf;
    }
    return s;
}

} // namespace

std::string describeMounted(uint16_t mounted)
{
    if (mounted == 0)
        return "No disks loaded";

    std::string s;
    int count = 0;
    for (int i = 0; i < NUM_DRIVES; ++i) {
        if (mounted & (1u << i)) {
            s += (count ? ", " : "") + std::to_string(i);
            ++count;
        }
    }
    return (count == 1 ? "Disk in drive " : "Disks in drives ") + s;
}

Client::~Client()
{
    stop();
}

void Client::start(std::unique_ptr<Transport> transport)
{
    stop();
    transport_ = std::move(transport);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        view_.running = true;
        view_.status = transport_->status();
        view_.readBusy = false;
        readPending_ = false;
    }
    running_ = true;
    thread_ = std::thread(&Client::run, this);
}

void Client::stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    wake_.notify_all();
    if (thread_.joinable())
        thread_.join();
    transport_.reset();

    std::lock_guard<std::mutex> lock(mutex_);
    view_.running = false;
    view_.readBusy = false;
    readPending_ = false;
}

void Client::setPolling(bool on)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        polling_ = on;
        pollNow_ = on;
        view_.polling = on;
    }
    wake_.notify_all();
}

void Client::requestRead(int drive, int track, int length)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || view_.readBusy)
            return;
        readPending_ = true;
        readDrive_ = drive;
        readTrack_ = track;
        readLength_ = length;
        view_.readBusy = true;
    }
    wake_.notify_all();
}

void Client::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    view_.statSent = view_.statAnswered = view_.statNoAnswer = view_.statBad = 0;
    view_.lastStat = StatResult::None;
    view_.lastStatMs = 0;
    view_.log.clear();
}

ClientView Client::view() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    ClientView v = view_;
    v.receiving = v.running && lastRx_ != Clock::time_point{}
        && Clock::now() - lastRx_ < RECEIVE_LIGHT_TIME;
    return v;
}

void Client::run()
{
    auto nextStat = Clock::now();

    while (running_) {
        bool readNow = false;
        int drive = 0, track = 0, length = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (pollNow_) {
                pollNow_ = false;
                nextStat = Clock::now();
            }
            if (readPending_) {
                readPending_ = false;
                readNow = true;
                drive = readDrive_;
                track = readTrack_;
                length = readLength_;
            } else if (!polling_ || Clock::now() < nextStat) {
                auto until = polling_ ? std::min(nextStat, Clock::now() + std::chrono::milliseconds(IDLE_WAIT_MS))
                                      : Clock::now() + std::chrono::milliseconds(IDLE_WAIT_MS);
                wake_.wait_until(lock, until);
                continue;
            }
        }

        if (readNow) {
            doRead(drive, track, length);
        } else {
            doStat();
            nextStat = Clock::now() + statInterval;
        }

        // Keep the receive light honest about stray bytes between commands.
        if (running_)
            flushInput();
    }
}

void Client::sendCommand(const char* name, uint16_t param1, uint16_t param2)
{
    uint8_t cmd[MSG_SIZE];
    std::memcpy(cmd, name, 4);
    putWord(cmd + 4, param1);
    putWord(cmd + 6, param2);
    putWord(cmd + 8, checksum(cmd, COMMAND_LENGTH));
    if (!transport_->write(cmd, sizeof(cmd)))
        failed(-1);
}

void Client::doStat()
{
    flushInput();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++view_.statSent;
    }

    auto start = Clock::now();
    sendCommand("STAT", NO_DRIVE_SELECTED, 0);
    if (!running_)
        return;

    uint8_t rsp[MSG_SIZE];
    size_t got = readExact(rsp, sizeof(rsp));
    int ms = msSince(start);

    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_)
        return;
    char num[32];
    std::snprintf(num, sizeof(num), "#%-6lu STAT  ", view_.statSent);
    view_.lastStatMs = ms;

    if (got == 0) {
        ++view_.statNoAnswer;
        view_.lastStat = StatResult::NoAnswer;
        addLog(std::string(num) + "NO ANSWER");
        return;
    }

    bool good = got == MSG_SIZE && std::memcmp(rsp, "STAT", 4) == 0
        && checksum(rsp, COMMAND_LENGTH) == getWord(rsp + 8);
    if (!good) {
        ++view_.statBad;
        view_.lastStat = StatResult::Bad;
        std::string why = got < MSG_SIZE
            ? "BAD ANSWER (only " + std::to_string(got) + " of 10 bytes): "
            : "BAD ANSWER: ";
        addLog(num + why + hexBytes(rsp, got));
        return;
    }

    ++view_.statAnswered;
    view_.lastStat = StatResult::Ok;
    view_.lastStatCode = getWord(rsp + 4);
    view_.mounted = getWord(rsp + 6);
    std::string code = view_.lastStatCode == 0
        ? "OK"
        : "CODE " + std::to_string(view_.lastStatCode);
    addLog(num + code + "  " + describeMounted(view_.mounted) + "  ("
           + std::to_string(ms) + " ms)");
}

void Client::doRead(int drive, int track, int length)
{
    std::vector<uint8_t> buf(static_cast<size_t>(length) + 2);

    flushInput();
    auto start = Clock::now();
    sendCommand("READ", static_cast<uint16_t>((drive << 12) | (track & 0x0fff)),
                static_cast<uint16_t>(length));
    size_t got = running_ ? readExact(buf.data(), buf.size()) : 0;

    ReadResult r;
    r.drive = drive;
    r.track = track;
    r.length = length;
    r.received = static_cast<int>(got);
    r.complete = got == buf.size();
    r.ms = msSince(start);
    if (r.complete) {
        r.expectedSum = checksum(buf.data(), static_cast<size_t>(length));
        r.receivedSum = getWord(buf.data() + length);
        r.checksumGood = r.expectedSum == r.receivedSum;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    view_.read = r;
    view_.readBusy = false;
    ++view_.readCount;

    std::string line = "        READ  drive " + std::to_string(drive) + " track " + std::to_string(track) + "  ";
    if (got == 0)
        line += "NO ANSWER";
    else if (!r.complete)
        line += "INCOMPLETE: " + std::to_string(got) + " of " + std::to_string(buf.size()) + " bytes";
    else
        line += std::to_string(length) + " bytes, checksum " + (r.checksumGood ? "GOOD" : "BAD")
            + "  (" + std::to_string(r.ms) + " ms)";
    addLog(line);
}

// Read len bytes. The timeout runs from the last byte received, so a long
// transfer at a low baud rate isn't cut off while data is still arriving.
// Returns the number of bytes received.
size_t Client::readExact(uint8_t* buf, size_t len)
{
    auto deadline = Clock::now() + answerTimeout;
    size_t got = 0;
    while (got < len && running_) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - Clock::now()).count();
        if (remaining <= 0)
            break;
        int n = transport_->read(buf + got, len - got,
                                 static_cast<int>(std::min<long long>(remaining, 100)));
        if (failed(n))
            break;
        if (n > 0) {
            deadline = Clock::now() + answerTimeout;
            got += static_cast<size_t>(n);
            std::lock_guard<std::mutex> lock(mutex_);
            lastRx_ = Clock::now();
        }
    }
    return got;
}

void Client::flushInput()
{
    uint8_t junk[256];
    for (;;) {
        int n = transport_->read(junk, sizeof(junk), 0);
        if (failed(n) || n == 0)
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        lastRx_ = Clock::now();
    }
}

bool Client::failed(int n)
{
    if (n >= 0)
        return false;
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    view_.running = false;
    view_.status = transport_->status();
    return true;
}

void Client::addLog(const std::string& line)
{
    view_.log.push_back(line);
    ++view_.logTotal;
    while (view_.log.size() > MAX_LOG)
        view_.log.pop_front();
}
