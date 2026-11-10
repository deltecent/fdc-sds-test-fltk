// Protocol client tests. A fake transport plays the serial drive server:
// it answers STAT and READ the way a server would, or misbehaves on request.

#include "../src/client.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr int TRACK_LEN = 4384;
constexpr int TRACKS = 77;

uint16_t sum(const uint8_t* p, size_t len)
{
    uint32_t s = 0;
    for (size_t i = 0; i < len; ++i)
        s += p[i];
    return static_cast<uint16_t>(s);
}

void putWord(std::vector<uint8_t>& v, uint16_t w)
{
    v.push_back(static_cast<uint8_t>(w & 0xff));
    v.push_back(static_cast<uint8_t>(w >> 8));
}

struct Server {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<uint8_t> toClient;

    // Behaviour
    bool silent = false;
    bool corruptStat = false;
    bool corruptTrack = false;
    int truncateTrack = 0;              // drop this many bytes from the end
    std::vector<uint8_t> images[NUM_DRIVES];

    int statCount = 0;
    int readCount = 0;
    uint16_t lastReadParam1 = 0;

    uint16_t mounted() const
    {
        uint16_t m = 0;
        for (int i = 0; i < NUM_DRIVES; ++i)
            if (!images[i].empty())
                m |= static_cast<uint16_t>(1u << i);
        return m;
    }

    // Called with mutex held.
    void handle(const uint8_t* cmd)
    {
        if (sum(cmd, 8) != static_cast<uint16_t>(cmd[8] | (cmd[9] << 8)))
            return;
        uint16_t p1 = static_cast<uint16_t>(cmd[4] | (cmd[5] << 8));
        uint16_t p2 = static_cast<uint16_t>(cmd[6] | (cmd[7] << 8));

        if (std::memcmp(cmd, "STAT", 4) == 0) {
            ++statCount;
            if (silent)
                return;
            std::vector<uint8_t> r = {'S', 'T', 'A', 'T'};
            putWord(r, 0);
            putWord(r, mounted());
            putWord(r, sum(r.data(), 8));
            if (corruptStat)
                r[9] ^= 0x55;
            toClient.insert(toClient.end(), r.begin(), r.end());
        } else if (std::memcmp(cmd, "READ", 4) == 0) {
            ++readCount;
            lastReadParam1 = p1;
            if (silent)
                return;
            int drive = p1 >> 12;
            size_t track = p1 & 0x0fff;
            const auto& img = images[drive];
            if (img.empty() || (track + 1) * p2 > img.size())
                return;
            std::vector<uint8_t> r(img.begin() + static_cast<long>(track * p2),
                                   img.begin() + static_cast<long>((track + 1) * p2));
            putWord(r, sum(r.data(), p2));
            if (corruptTrack)
                r[100] ^= 0x01;
            r.resize(r.size() - static_cast<size_t>(truncateTrack));
            toClient.insert(toClient.end(), r.begin(), r.end());
        }
        cv.notify_all();
    }
};

class FakeTransport : public Transport {
public:
    explicit FakeTransport(Server& s) : s_(s) {}

    int read(uint8_t* buf, size_t len, int timeoutMs) override
    {
        std::unique_lock<std::mutex> lock(s_.mutex);
        s_.cv.wait_for(lock, milliseconds(timeoutMs), [&] { return !s_.toClient.empty(); });
        size_t n = 0;
        while (n < len && !s_.toClient.empty()) {
            buf[n++] = s_.toClient.front();
            s_.toClient.pop_front();
        }
        return static_cast<int>(n);
    }

    bool write(const uint8_t* buf, size_t len) override
    {
        std::lock_guard<std::mutex> lock(s_.mutex);
        pending_.insert(pending_.end(), buf, buf + len);
        while (pending_.size() >= 10) {
            s_.handle(pending_.data());
            pending_.erase(pending_.begin(), pending_.begin() + 10);
        }
        return true;
    }

    std::string status() const override { return "test"; }

private:
    Server& s_;
    std::vector<uint8_t> pending_;
};

int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++failures;
}

bool waitFor(const std::function<bool()>& cond, int ms = 5000)
{
    auto deadline = Clock::now() + milliseconds(ms);
    while (Clock::now() < deadline) {
        if (cond())
            return true;
        std::this_thread::sleep_for(milliseconds(5));
    }
    return cond();
}

std::vector<uint8_t> makeImage(uint8_t seed)
{
    std::vector<uint8_t> img(static_cast<size_t>(TRACK_LEN) * TRACKS);
    for (size_t i = 0; i < img.size(); ++i)
        img[i] = static_cast<uint8_t>(i * 7 + seed);
    return img;
}

struct Fixture {
    Server server;
    Client client;

    Fixture()
    {
        client.statInterval = milliseconds(20);
        client.answerTimeout = milliseconds(200);
    }

    void start() { client.start(std::make_unique<FakeTransport>(server)); }

    ReadResult read(int drive, int track, int length = TRACK_LEN)
    {
        unsigned long before = client.view().readCount;
        client.requestRead(drive, track, length);
        waitFor([&] { return client.view().readCount != before; });
        return client.view().read;
    }
};

void testStatAnswered()
{
    Fixture f;
    f.server.images[0] = makeImage(1);
    f.server.images[2] = makeImage(2);
    f.start();
    f.client.setPolling(true);
    bool ok = waitFor([&] { return f.client.view().statAnswered >= 5; });
    f.client.setPolling(false);
    ClientView v = f.client.view();
    check(ok, "STAT answers are counted");
    check(v.mounted == 0x0005, "STAT reports mounted drives 0 and 2");
    check(v.lastStat == StatResult::Ok, "last STAT is OK");
    check(v.statNoAnswer == 0 && v.statBad == 0, "no STAT failures");
    check(!v.log.empty() && v.log.back().find("Disks in drives 0, 2") != std::string::npos,
          "log describes mounted drives");
}

void testStatNoAnswer()
{
    Fixture f;
    f.server.silent = true;
    f.start();
    f.client.setPolling(true);
    bool ok = waitFor([&] { return f.client.view().statNoAnswer >= 2; });
    f.client.setPolling(false);
    ClientView v = f.client.view();
    check(ok, "silent server counts as no answer");
    check(v.statAnswered == 0, "nothing answered");
    check(v.lastStat == StatResult::NoAnswer, "last STAT is no answer");
}

void testStatBad()
{
    Fixture f;
    f.server.corruptStat = true;
    f.start();
    f.client.setPolling(true);
    bool ok = waitFor([&] { return f.client.view().statBad >= 2; });
    f.client.setPolling(false);
    check(ok, "corrupt STAT reply counts as bad answer");
    check(f.client.view().lastStat == StatResult::Bad, "last STAT is bad");
}

void testReadGood()
{
    Fixture f;
    f.server.images[1] = makeImage(9);
    f.start();
    ReadResult r = f.read(1, 5);
    check(r.complete && r.checksumGood, "track read with good checksum");
    check(r.received == TRACK_LEN + 2, "whole track and checksum received");
    check(f.server.lastReadParam1 == ((1 << 12) | 5), "READ encodes drive and track");
}

void testReadBadChecksum()
{
    Fixture f;
    f.server.images[0] = makeImage(3);
    f.server.corruptTrack = true;
    f.start();
    ReadResult r = f.read(0, 0);
    check(r.complete && !r.checksumGood, "corrupted track has bad checksum");
}

void testReadIncomplete()
{
    Fixture f;
    f.server.images[0] = makeImage(3);
    f.server.truncateTrack = 50;
    f.start();
    ReadResult r = f.read(0, 0);
    check(!r.complete && r.received == TRACK_LEN + 2 - 50, "short track reported incomplete");
}

void testReadUnmounted()
{
    Fixture f;
    f.server.images[0] = makeImage(3);
    f.start();
    ReadResult r = f.read(3, 0);
    check(r.received == 0 && !r.complete, "unmounted drive gives no answer");
    r = f.read(0, TRACKS);
    check(r.received == 0, "track past end of image gives no answer");
}

void testReadWhilePolling()
{
    Fixture f;
    f.server.images[0] = makeImage(4);
    f.start();
    f.client.setPolling(true);
    waitFor([&] { return f.client.view().statAnswered >= 3; });
    ReadResult r = f.read(0, 10);
    check(r.complete && r.checksumGood, "read during polling succeeds");
    unsigned long answered = f.client.view().statAnswered;
    bool ok = waitFor([&] { return f.client.view().statAnswered > answered + 2; });
    ClientView v = f.client.view();
    f.client.setPolling(false);
    check(ok, "polling resumes after read");
    check(v.statBad == 0 && v.statNoAnswer == 0, "read does not disturb STAT");
}

void testClear()
{
    Fixture f;
    f.start();
    f.client.setPolling(true);
    waitFor([&] { return f.client.view().statAnswered >= 2; });
    f.client.setPolling(false);
    std::this_thread::sleep_for(milliseconds(50));
    f.client.clear();
    ClientView v = f.client.view();
    check(v.statSent == 0 && v.statAnswered == 0 && v.log.empty(), "clear resets counts and log");
}

void testDescribe()
{
    check(describeMounted(0) == "No disks loaded", "describe no disks");
    check(describeMounted(1) == "Disk in drive 0", "describe one disk");
    check(describeMounted(0x8003) == "Disks in drives 0, 1, 15", "describe several disks");
}

} // namespace

int main()
{
    testDescribe();
    testStatAnswered();
    testStatNoAnswer();
    testStatBad();
    testReadGood();
    testReadBadChecksum();
    testReadIncomplete();
    testReadUnmounted();
    testReadWhilePolling();
    testClear();

    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
