// FDC+ Server Tester - acts as an FDC+ Enhanced Floppy Disk Controller so a
// serial drive server can be tested without an Altair.

#include "client.h"
#include "transport.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Browser.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Preferences.H>
#include <FL/Fl_Spinner.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr int WIN_W = 640;
constexpr int MARGIN = 8;
constexpr int BIG_BUTTON_H = 40;
constexpr int STAT_PANEL_Y = 84;
constexpr int STAT_PANEL_H = 320;
constexpr int READ_PANEL_Y = STAT_PANEL_Y + STAT_PANEL_H + MARGIN;
constexpr int READ_PANEL_H = 168;
constexpr int WIN_H = READ_PANEL_Y + READ_PANEL_H + 32;
constexpr int MAX_LOG_LINES = 200;
constexpr double REFRESH_SECONDS = 0.05;

// Approximation of the MITS Altair 8800 case blue.
const Fl_Color ALTAIR_BLUE = fl_rgb_color(0x4A, 0x7A, 0xB0);
const Fl_Color ALTAIR_BLUE_DARK = fl_rgb_color(0x35, 0x5A, 0x85);
const Fl_Color LED_OFF = fl_rgb_color(128, 128, 128);
const Fl_Color LED_GREEN = fl_rgb_color(0, 255, 0);
const Fl_Color TEXT_GOOD = fl_rgb_color(140, 255, 140);
const Fl_Color TEXT_BAD = fl_rgb_color(255, 225, 90);
const Fl_Color BUTTON_GO = fl_rgb_color(60, 170, 60);
const Fl_Color BUTTON_STOP = fl_rgb_color(200, 60, 50);

struct BaudRate {
    int rate;
    const char* label;
};

const BaudRate BAUD_RATES[] = {
    {403200, "403.2K"}, {460800, "460.8K"}, {230400, "230.4K"}, {76800, "76.8K"},
    {57600, "57.6K"},   {38400, "38.4K"},   {19200, "19.2K"},   {9600, "9.6K"},
};
constexpr int NUM_BAUD_RATES = sizeof(BAUD_RATES) / sizeof(BAUD_RATES[0]);

// Altair disks have 137 byte sectors.
struct DiskType {
    const char* label;
    int trackLength;
    int maxTrack;
};

const DiskType DISK_TYPES[] = {
    {"8\" floppy", 32 * 137, 76},
    {"Minidisk", 16 * 137, 34},
    {"8 MB serial disk", 32 * 137, 2047},
};
constexpr int NUM_DISK_TYPES = sizeof(DISK_TYPES) / sizeof(DISK_TYPES[0]);

// Round indicator lamp.
class Led : public Fl_Widget {
public:
    Led(int x, int y, int d) : Fl_Widget(x, y, d, d) {}

    void set(Fl_Color c)
    {
        if (c != color()) {
            color(c);
            redraw();
        }
    }

protected:
    void draw() override
    {
        fl_color(color());
        fl_pie(x(), y(), w(), h(), 0, 360);
        fl_color(FL_DARK3);
        fl_arc(x(), y(), w(), h(), 0, 360);
    }
};

Fl_Box* whiteLabel(int x, int y, int w, int h, const char* text, int size = 14)
{
    auto* b = new Fl_Box(x, y, w, h, text);
    b->labelcolor(FL_WHITE);
    b->labelsize(size);
    b->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    return b;
}

Fl_Box* panelFrame(int y, int h, const char* title)
{
    auto* frame = new Fl_Box(FL_ENGRAVED_FRAME, MARGIN, y, WIN_W - 2 * MARGIN, h, nullptr);
    frame->color(ALTAIR_BLUE);
    auto* t = whiteLabel(MARGIN + 10, y + 6, WIN_W - 40, 24, title, 16);
    t->labelfont(FL_HELVETICA_BOLD);
    return t;
}

// Set a box's label only if it changed, to avoid needless redraws.
void setLabel(Fl_Widget* w, const std::string& text)
{
    if (!w->label() || text != w->label())
        w->copy_label(text.c_str());
}

void setLabelColor(Fl_Widget* w, Fl_Color c)
{
    if (w->labelcolor() != c) {
        w->labelcolor(c);
        w->redraw_label();
    }
}

// Escape characters Fl_Menu_::add() treats specially (e.g. '/' makes submenus).
std::string menuEscape(const std::string& s)
{
    std::string out;
    for (char c : s) {
        if (c == '/' || c == '\\' || c == '&' || c == '_')
            out += '\\';
        out += c;
    }
    return out;
}

// A counter: small caption above a large number.
struct Counter {
    Fl_Box* value = nullptr;

    Counter(int x, int y, int w, const char* caption)
    {
        whiteLabel(x, y, w, 18, caption, 12);
        value = whiteLabel(x, y + 18, w, 26, "0", 22);
        value->labelfont(FL_HELVETICA_BOLD);
    }

    void set(const std::string& text) { setLabel(value, text); }
};

class App {
public:
    App();
    int run(int argc, char** argv);

private:
    void buildWindow();
    void buildStatPanel();
    void buildReadPanel();
    void loadPreferences();
    void savePreferences();

    void scanSerialPorts();
    void connect(bool reportErrors);
    void updateTrackRange();
    void refresh();
    void refreshStat(const ClientView& v);
    void refreshRead(const ClientView& v);
    void close();

    static void onConnectionChange(Fl_Widget*, void* app) { static_cast<App*>(app)->connect(true); }
    static void onRescan(Fl_Widget*, void* app);
    static void onStartStop(Fl_Widget*, void* app);
    static void onClear(Fl_Widget*, void* app);
    static void onDiskType(Fl_Widget*, void* app) { static_cast<App*>(app)->updateTrackRange(); }
    static void onReadTrack(Fl_Widget*, void* app);
    static void onAbout(Fl_Widget*, void*);
    static void onClose(Fl_Widget*, void* app) { static_cast<App*>(app)->close(); }
    static void onTimer(void* app);

    Client client_;
    Fl_Preferences prefs_;

    Fl_Double_Window* window_ = nullptr;
    Fl_Choice* serialPort_ = nullptr;
    Fl_Choice* baud_ = nullptr;
    Led* rxLed_ = nullptr;
    Fl_Box* status_ = nullptr;

    Fl_Button* startStop_ = nullptr;
    Fl_Button* clear_ = nullptr;
    Fl_Box* statMessage_ = nullptr;
    Counter* sent_ = nullptr;
    Counter* answered_ = nullptr;
    Counter* noAnswer_ = nullptr;
    Counter* badAnswer_ = nullptr;
    Counter* replyTime_ = nullptr;
    Led* driveLeds_[NUM_DRIVES] = {};
    Fl_Browser* log_ = nullptr;
    unsigned long logShown_ = 0;

    Fl_Choice* drive_ = nullptr;
    Fl_Choice* diskType_ = nullptr;
    Fl_Spinner* track_ = nullptr;
    Fl_Button* readTrack_ = nullptr;
    Fl_Box* readMessage_ = nullptr;
    Fl_Box* readDetail_ = nullptr;

    std::vector<std::string> portNames_;
    std::string serialName_;
    std::string errorStatus_;
    int winX_ = -1, winY_ = -1;
};

App::App() : prefs_(Fl_Preferences::USER_L, "deltecent.com", "fdcsdstest") {}

int App::run(int argc, char** argv)
{
    Fl::scheme("gtk+");
    FL_NORMAL_SIZE = 14;
    buildWindow();
    loadPreferences();
    scanSerialPorts();
    updateTrackRange();

    if (winX_ >= 0 && winY_ >= 0) {
        int sx, sy, sw, sh;
        Fl::screen_work_area(sx, sy, sw, sh, winX_, winY_);
        if (winX_ >= sx && winY_ >= sy && winX_ < sx + sw - 50 && winY_ < sy + sh - 50)
            window_->position(winX_, winY_);
    }
    window_->show(argc, argv);

    connect(false);
    refresh();
    Fl::add_timeout(REFRESH_SECONDS, onTimer, this);
    return Fl::run();
}

void App::buildWindow()
{
    window_ = new Fl_Double_Window(WIN_W, WIN_H, "FDC+ Server Tester");
    window_->color(ALTAIR_BLUE);
    window_->callback(onClose, this);

    static const std::string title = std::string("FDC+ Server Tester v") + FDCSDSTEST_VERSION;
    whiteLabel(MARGIN + 2, 8, 300, 24, title.c_str(), 18)->labelfont(FL_HELVETICA_BOLD);
    auto* about = new Fl_Button(WIN_W - 200, 8, 70, 26, "About");
    about->callback(onAbout, this);

    whiteLabel(WIN_W - 112, 8, 70, 26, "Receive");
    rxLed_ = new Led(WIN_W - 34, 13, 16);
    rxLed_->color(LED_OFF);

    whiteLabel(MARGIN + 2, 44, 80, 30, "Serial port");
    serialPort_ = new Fl_Choice(92, 44, 250, 30);
    serialPort_->tooltip("The serial port the drive server is connected to");
    serialPort_->callback(onConnectionChange, this);

    whiteLabel(354, 44, 50, 30, "Speed");
    baud_ = new Fl_Choice(404, 44, 100, 30);
    baud_->tooltip("Must be the same speed (baud rate) the server uses");
    for (const auto& b : BAUD_RATES)
        baud_->add(b.label);
    baud_->value(0);
    baud_->callback(onConnectionChange, this);

    auto* rescan = new Fl_Button(WIN_W - MARGIN - 112, 44, 112, 30, "Find Ports");
    rescan->tooltip("Look again for serial ports, e.g. after plugging in a USB adapter");
    rescan->callback(onRescan, this);

    buildStatPanel();
    buildReadPanel();

    status_ = whiteLabel(MARGIN + 2, WIN_H - 26, WIN_W - 220, 22, "", 13);
    whiteLabel(WIN_W - 210, WIN_H - 26, 200, 22, "\xC2\xA9" "2026 Deltec Enterprises LLC", 12)
        ->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);

    window_->end();
}

void App::buildStatPanel()
{
    const int y = STAT_PANEL_Y;
    const int left = MARGIN + 12;
    panelFrame(y, STAT_PANEL_H, "Step 1:  Is the server answering?");

    startStop_ = new Fl_Button(left, y + 36, 150, BIG_BUTTON_H, "START");
    startStop_->labelsize(18);
    startStop_->labelfont(FL_HELVETICA_BOLD);
    startStop_->labelcolor(FL_WHITE);
    startStop_->color(BUTTON_GO);
    startStop_->tooltip("Start or stop asking the server for its status");
    startStop_->callback(onStartStop, this);

    statMessage_ = whiteLabel(left + 162, y + 36, WIN_W - left - 162 - 120, BIG_BUTTON_H, "", 15);
    statMessage_->labelfont(FL_HELVETICA_BOLD);
    statMessage_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);

    clear_ = new Fl_Button(WIN_W - MARGIN - 12 - 100, y + 36, 100, BIG_BUTTON_H, "Clear");
    clear_->tooltip("Set the counts back to zero and empty the list");
    clear_->callback(onClear, this);

    const int cy = y + 86;
    const int cw = (WIN_W - 2 * left) / 5;
    sent_ = new Counter(left, cy, cw, "Sent");
    answered_ = new Counter(left + cw, cy, cw, "Answered");
    noAnswer_ = new Counter(left + 2 * cw, cy, cw, "No answer");
    badAnswer_ = new Counter(left + 3 * cw, cy, cw, "Bad answer");
    replyTime_ = new Counter(left + 4 * cw, cy, cw, "Reply time");
    replyTime_->set("-");

    const int ly = y + 140;
    whiteLabel(left, ly, 110, 20, "Disks loaded:");
    for (int i = 0; i < NUM_DRIVES; ++i) {
        int x = left + 112 + i * 30;
        driveLeds_[i] = new Led(x + 4, ly + 2, 16);
        driveLeds_[i]->color(LED_OFF);
        driveLeds_[i]->tooltip("Green when the server has a disk loaded in this drive");
        auto* n = whiteLabel(x, ly + 20, 24, 16, nullptr, 11);
        n->copy_label(std::to_string(i).c_str());
        n->align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
    }

    log_ = new Fl_Browser(left, y + 184, WIN_W - 2 * left, STAT_PANEL_H - 184 - 10);
    log_->textfont(FL_COURIER);
    log_->textsize(13);
    log_->color(fl_rgb_color(250, 250, 250));
}

void App::buildReadPanel()
{
    const int y = READ_PANEL_Y;
    const int left = MARGIN + 12;
    panelFrame(y, READ_PANEL_H, "Step 2:  Read a track from a disk");

    const int row = y + 58;
    drive_ = new Fl_Choice(left, row, 70, BIG_BUTTON_H, "Drive");
    for (int i = 0; i < NUM_DRIVES; ++i)
        drive_->add(std::to_string(i).c_str());
    drive_->value(0);
    drive_->tooltip("The drive number the disk is loaded in on the server");

    diskType_ = new Fl_Choice(left + 84, row, 180, BIG_BUTTON_H, "Disk type");
    for (const auto& d : DISK_TYPES)
        diskType_->add(menuEscape(d.label).c_str());
    diskType_->value(0);
    diskType_->tooltip("The kind of disk image loaded in that drive");
    diskType_->callback(onDiskType, this);

    track_ = new Fl_Spinner(left + 278, row, 100, BIG_BUTTON_H, "Track");
    track_->type(FL_INT_INPUT);
    track_->step(1);
    track_->value(0);
    track_->textsize(16);
    track_->tooltip("Track number to read (0 is the first track)");

    for (Fl_Widget* w : {static_cast<Fl_Widget*>(drive_), static_cast<Fl_Widget*>(diskType_),
                         static_cast<Fl_Widget*>(track_)}) {
        w->align(FL_ALIGN_TOP_LEFT);
        w->labelcolor(FL_WHITE);
        w->labelsize(13);
    }

    readTrack_ = new Fl_Button(WIN_W - MARGIN - 12 - 190, row, 190, BIG_BUTTON_H, "READ TRACK");
    readTrack_->labelsize(18);
    readTrack_->labelfont(FL_HELVETICA_BOLD);
    readTrack_->labelcolor(FL_WHITE);
    readTrack_->color(BUTTON_GO);
    readTrack_->tooltip("Ask the server for this track and check that it arrives intact");
    readTrack_->callback(onReadTrack, this);

    readMessage_ = whiteLabel(left, row + BIG_BUTTON_H + 10, WIN_W - 2 * left, 24, "", 15);
    readMessage_->labelfont(FL_HELVETICA_BOLD);
    readDetail_ = whiteLabel(left, row + BIG_BUTTON_H + 34, WIN_W - 2 * left, 20, "", 13);
}

void App::loadPreferences()
{
    char buf[1024];
    int value;

    prefs_.get("SerialPort", buf, "", sizeof(buf));
    serialName_ = buf;

    prefs_.get("BaudIndex", value, 0);
    baud_->value(value >= 0 && value < NUM_BAUD_RATES ? value : 0);

    prefs_.get("Drive", value, 0);
    drive_->value(value >= 0 && value < NUM_DRIVES ? value : 0);

    prefs_.get("DiskType", value, 0);
    diskType_->value(value >= 0 && value < NUM_DISK_TYPES ? value : 0);

    prefs_.get("Track", value, 0);
    track_->value(value);

    prefs_.get("WindowX", winX_, -1);
    prefs_.get("WindowY", winY_, -1);
}

void App::savePreferences()
{
    if (!serialName_.empty())
        prefs_.set("SerialPort", serialName_.c_str());
    prefs_.set("BaudIndex", baud_->value());
    prefs_.set("Drive", drive_->value());
    prefs_.set("DiskType", diskType_->value());
    prefs_.set("Track", static_cast<int>(track_->value()));
    prefs_.set("WindowX", window_->x());
    prefs_.set("WindowY", window_->y());
    prefs_.flush();
}

void App::scanSerialPorts()
{
    portNames_ = listSerialPorts();
    serialPort_->clear();
    for (const auto& name : portNames_)
        serialPort_->add(menuEscape(name).c_str());

    serialPort_->value(-1);
    for (size_t i = 0; i < portNames_.size(); ++i)
        if (portNames_[i] == serialName_)
            serialPort_->value(static_cast<int>(i));
    serialPort_->redraw();
}

void App::connect(bool reportErrors)
{
    client_.stop();

    int idx = serialPort_->value();
    if (idx < 0 || idx >= static_cast<int>(portNames_.size())) {
        errorStatus_ = portNames_.empty() ? "No serial ports found. Plug in the cable, then click Find Ports."
                                          : "Choose the serial port the server is connected to.";
        return;
    }
    serialName_ = portNames_[idx];
    const BaudRate& b = BAUD_RATES[baud_->value() < 0 ? 0 : baud_->value()];

    std::string error;
    auto transport = openSerial(serialName_, b.rate, b.label, error);
    if (!transport) {
        errorStatus_ = error;
        if (reportErrors)
            fl_alert("%s\n\nIs another program (such as the drive server) using this port?",
                     error.c_str());
        return;
    }

    errorStatus_.clear();
    client_.start(std::move(transport));
}

void App::updateTrackRange()
{
    int t = diskType_->value() < 0 ? 0 : diskType_->value();
    track_->range(0, DISK_TYPES[t].maxTrack);
    if (track_->value() > DISK_TYPES[t].maxTrack)
        track_->value(DISK_TYPES[t].maxTrack);
    if (track_->value() < 0)
        track_->value(0);
}

void App::onRescan(Fl_Widget*, void* app)
{
    auto* self = static_cast<App*>(app);
    self->scanSerialPorts();
    self->connect(false);
}

void App::onStartStop(Fl_Widget*, void* app)
{
    auto* self = static_cast<App*>(app);
    self->client_.setPolling(!self->client_.view().polling);
    self->refresh();
}

void App::onClear(Fl_Widget*, void* app)
{
    auto* self = static_cast<App*>(app);
    self->client_.clear();
    self->log_->clear();
    self->refresh();
}

void App::onReadTrack(Fl_Widget*, void* app)
{
    auto* self = static_cast<App*>(app);
    self->updateTrackRange();
    int type = self->diskType_->value() < 0 ? 0 : self->diskType_->value();
    self->client_.requestRead(self->drive_->value(), static_cast<int>(self->track_->value()),
                              DISK_TYPES[type].trackLength);
    self->refresh();
}

void App::onAbout(Fl_Widget*, void*)
{
    fl_message_title("About FDC+ Server Tester");
    fl_message("FDC+ Server Tester\n"
               "Version %s\n"
               "\xC2\xA9 2026 Deltec Enterprises LLC\n"
               "\n"
               "Acts as an FDC+ disk controller to test an\n"
               "FDC+ Serial Drive Server. It only reads from\n"
               "the server and never changes your disks.",
               FDCSDSTEST_VERSION);
}

// Copy the client's state onto the widgets.
void App::refresh()
{
    ClientView v = client_.view();

    rxLed_->set(v.receiving ? LED_GREEN : LED_OFF);

    std::string status = v.running ? v.status : errorStatus_;
    if (status.empty())
        status = v.status;
    if (!v.running && status.empty())
        status = "Not connected";
    setLabel(status_, status);

    refreshStat(v);
    refreshRead(v);
}

void App::refreshStat(const ClientView& v)
{
    if (v.running)
        startStop_->activate();
    else
        startStop_->deactivate();

    const char* label = v.polling ? "STOP" : "START";
    if (std::string(startStop_->label()) != label) {
        startStop_->label(label);
        startStop_->color(v.polling ? BUTTON_STOP : BUTTON_GO);
        startStop_->redraw();
    }

    sent_->set(std::to_string(v.statSent));
    answered_->set(std::to_string(v.statAnswered));
    noAnswer_->set(std::to_string(v.statNoAnswer));
    badAnswer_->set(std::to_string(v.statBad));
    replyTime_->set(v.lastStat == StatResult::Ok ? std::to_string(v.lastStatMs) + " ms" : "-");

    std::string message;
    Fl_Color color = FL_WHITE;
    if (!v.running) {
        message = "Choose the serial port and speed above.";
    } else {
        switch (v.lastStat) {
        case StatResult::None:
            message = v.polling ? "Asking the server..." : "Press START to begin.";
            break;
        case StatResult::Ok:
            if (v.lastStatCode == 0) {
                message = "Yes! The server is answering.";
                color = TEXT_GOOD;
            } else {
                message = "The server answered with error code " + std::to_string(v.lastStatCode) + ".";
                color = TEXT_BAD;
            }
            break;
        case StatResult::NoAnswer:
            message = "No answer. Check the cable, the serial port, and the speed.";
            color = TEXT_BAD;
            break;
        case StatResult::Bad:
            message = "Garbled answer. The speed probably does not match the server.";
            color = TEXT_BAD;
            break;
        }
    }
    setLabel(statMessage_, message);
    setLabelColor(statMessage_, color);

    bool haveDrives = v.running && v.statAnswered > 0;
    for (int i = 0; i < NUM_DRIVES; ++i)
        driveLeds_[i]->set(haveDrives && (v.mounted & (1u << i)) ? LED_GREEN : LED_OFF);

    // Append new log lines, keeping the newest in view.
    if (v.logTotal != logShown_) {
        unsigned long fresh = v.logTotal - logShown_;
        size_t first = fresh >= v.log.size() ? 0 : v.log.size() - fresh;
        for (size_t i = first; i < v.log.size(); ++i)
            log_->add(v.log[i].c_str());
        while (log_->size() > MAX_LOG_LINES)
            log_->remove(1);
        log_->bottomline(log_->size());
        logShown_ = v.logTotal;
    }
}

void App::refreshRead(const ClientView& v)
{
    if (v.running && !v.readBusy)
        readTrack_->activate();
    else
        readTrack_->deactivate();

    if (v.readBusy) {
        setLabel(readMessage_, "Reading...");
        setLabelColor(readMessage_, FL_WHITE);
        setLabel(readDetail_, "");
        return;
    }
    if (v.readCount == 0) {
        setLabel(readMessage_, "Choose a drive and track, then press READ TRACK.");
        setLabelColor(readMessage_, FL_WHITE);
        setLabel(readDetail_, "The tester only reads. It never changes your disks.");
        return;
    }

    const ReadResult& r = v.read;
    std::string where = "track " + std::to_string(r.track) + " of drive " + std::to_string(r.drive);
    std::string message;
    std::string detail;
    bool good = false;
    char sums[96];

    if (r.received == 0 && v.statAnswered > 0 && !(v.mounted & (1u << r.drive))) {
        message = "No answer. The server has no disk loaded in drive " + std::to_string(r.drive) + ".";
        detail = "Load a disk image in that drive on the server, or choose another drive.";
    } else if (r.received == 0) {
        message = "No answer. Is a disk loaded in drive " + std::to_string(r.drive) + "?";
        detail = "Also check the disk type, and that the disk has track " + std::to_string(r.track) + ".";
    } else if (!r.complete) {
        message = "Only part of " + where + " arrived.";
        detail = std::to_string(r.received) + " of " + std::to_string(r.length + 2)
            + " bytes. Check the disk type, or try a slower speed.";
    } else if (!r.checksumGood) {
        message = "Read " + where + ", but the checksum is BAD.";
        std::snprintf(sums, sizeof(sums), "Server sent %04X, data adds up to %04X. ",
                      r.receivedSum, r.expectedSum);
        detail = std::string(sums) + "Data was damaged on the way.";
    } else {
        message = "Read " + where + " OK. Checksum good.";
        std::snprintf(sums, sizeof(sums), "%d bytes in %d ms, checksum %04X.",
                      r.length, r.ms, r.receivedSum);
        detail = sums;
        good = true;
    }
    setLabel(readMessage_, message);
    setLabelColor(readMessage_, good ? TEXT_GOOD : TEXT_BAD);
    setLabel(readDetail_, detail);
}

void App::onTimer(void* app)
{
    static_cast<App*>(app)->refresh();
    Fl::repeat_timeout(REFRESH_SECONDS, onTimer, app);
}

void App::close()
{
    // Escape closes FLTK windows by default; only close on a real close request.
    if (Fl::event() == FL_SHORTCUT && Fl::event_key() == FL_Escape)
        return;

    savePreferences();
    client_.stop();
    window_->hide();
}

} // namespace

int main(int argc, char** argv)
{
    App app;
    return app.run(argc, argv);
}
