// NSFPlay for Linux - playback engine (xgm core + ALSA output thread)
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../xgm/xgm.h"

// Config subclass that exposes key iteration for persistence / advanced editor.
class NsfConfig : public xgm::NSFPlayerConfig {
public:
    std::vector<std::string> Keys() {
        MutexGuard g(this);
        std::vector<std::string> k;
        for (auto &kv : data) k.push_back(kv.first);
        return k;
    }
};

struct TrackEntry {
    int index;          // 0-based song index (playlist order)
    std::string label;  // NSFe track label, may be empty
    int length_ms;      // play length incl. fade
};

struct ChannelState {
    bool valid = false;
    int volume = 0;
    int max_volume = 1;
    bool key = false;
    double freq = 0.0;
};

class Engine {
public:
    Engine();
    ~Engine();

    NsfConfig cfg;

    bool Open(const std::string &path, std::string &err);
    bool IsLoaded() const { return loaded; }

    // header info (valid after Open)
    std::string title, artist, copyright, ripper, text, filename;
    std::vector<TrackEntry> tracks;
    bool use_fds = false, use_mmc5 = false, use_fme7 = false, use_vrc6 = false, use_vrc7 = false, use_n163 = false;
    int region_flags = 0; // bit0 NTSC, bit1 PAL, bit2 Dendy
    int nsf_version = 0;
    int load_addr = 0, init_addr = 0, play_addr = 0;
    bool is_nsfe = false;

    void PlayTrack(int song);   // 0-based, restarts
    void Play();                // resume / start current
    void Pause();
    void Stop();
    void Seek(int ms);
    bool IsPlaying() const { return playing && !paused; }
    bool IsActive() const { return playing; }
    bool IsPaused() const { return paused; }
    int CurrentTrack() const { return cur_song; }
    int PositionMs();           // audible position
    int LengthMs();             // current track length (incl. fade)
    bool ConsumeEnded();        // true once when track finished

    // Apply config changes to the running player.
    void ApplyConfig();
    // Recompute track lengths after time settings changed.
    void RefreshTrackLengths();
    // Called when RATE changed: reopen device.
    void ReopenAudio() { reopen = true; }
    std::string AudioError();

    void GetChannels(std::vector<ChannelState> &out);

    // Offline WAV render of a track; progress callback returns false to cancel.
    bool ExportWav(int song, const std::string &out, std::string &err,
                   const std::function<bool(double)> &progress);

    void LoadSettings(const std::string &ini);
    void SaveSettings(const std::string &ini);

private:
    void Thread();
    void ResetLocked();
    void ApplyDefaultsLocked();

    std::recursive_mutex m;
    xgm::NSF nsf;
    xgm::NSFPlayer player;
    bool loaded = false;

    std::thread th;
    std::atomic<bool> quit{false}, playing{false}, paused{false}, ended{false}, reopen{true};
    std::atomic<int> cur_song{0};
    std::atomic<long long> frames_rendered{0};
    std::atomic<long long> delay_frames{0};
    std::atomic<int> rate{48000};
    std::mutex errm;
    std::string audio_err;
};
