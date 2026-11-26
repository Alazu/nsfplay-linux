// NSFPlay for Linux - playback engine
#include "engine.h"

#include <alsa/asoundlib.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>

using Lock = std::lock_guard<std::recursive_mutex>;
using xgm::INT16;
using xgm::UINT32;

// per-file timing values restored at each track start (auto-detection mutates them)
static int s_time = -1, s_loop = -1, s_fade = -1;
static bool s_unknown = true;

Engine::Engine() {
    cfg["MASTER_VOLUME"] = 128;
    player.SetConfig(&cfg);
    th = std::thread(&Engine::Thread, this);
}

Engine::~Engine() {
    quit = true;
    if (th.joinable()) th.join();
}

void Engine::ApplyDefaultsLocked() {
    nsf.SetDefaults(cfg["PLAY_TIME"].GetInt(), cfg["FADE_TIME"].GetInt(), cfg["LOOP_NUM"].GetInt());
}

bool Engine::Open(const std::string &path, std::string &err) {
    Lock g(m);
    playing = false;
    paused = false;
    loaded = false;
    if (!nsf.LoadFile(path.c_str())) {
        err = nsf.LoadError();
        return false;
    }
    ApplyDefaultsLocked();
    if (!player.Load(&nsf)) {
        err = "Player failed to load the file.";
        return false;
    }
    s_time = nsf.time_in_ms;
    s_loop = nsf.loop_in_ms;
    s_fade = nsf.fade_in_ms;
    s_unknown = nsf.playtime_unknown;

    filename = path;
    title = nsf.title ? nsf.title : "";
    artist = nsf.artist ? nsf.artist : "";
    copyright = nsf.copyright ? nsf.copyright : "";
    ripper = nsf.ripper ? nsf.ripper : "";
    text = (nsf.text && nsf.text_len) ? std::string(nsf.text, strnlen(nsf.text, nsf.text_len)) : "";
    use_fds = nsf.use_fds; use_mmc5 = nsf.use_mmc5; use_fme7 = nsf.use_fme7;
    use_vrc6 = nsf.use_vrc6; use_vrc7 = nsf.use_vrc7; use_n163 = nsf.use_n106;
    nsf_version = nsf.version;
    load_addr = nsf.load_address; init_addr = nsf.init_address; play_addr = nsf.play_address;
    is_nsfe = std::string(nsf.magic, 4) == "NSFE";
    // region flags: NSF pal_ntsc bit0 = PAL, bit1 = dual; NSFe/NSF2 regn chunk
    if (nsf.regn) region_flags = nsf.regn;
    else if (nsf.pal_ntsc & 2) region_flags = 3;
    else if (nsf.pal_ntsc & 1) region_flags = 2;
    else region_flags = 1;

    int start = nsf.playlist_mode ? nsf.song : (nsf.start > 0 ? nsf.start - 1 : 0);
    if (start >= nsf.songs) start = 0;
    loaded = true;
    RefreshTrackLengths();
    cur_song = start;
    nsf.SetSong(start);
    return true;
}

void Engine::RefreshTrackLengths() {
    Lock g(m);
    if (!loaded) return;
    ApplyDefaultsLocked();
    int saved = nsf.song;
    int st = nsf.time_in_ms, sl = nsf.loop_in_ms, sf = nsf.fade_in_ms;
    nsf.time_in_ms = s_time; nsf.loop_in_ms = s_loop; nsf.fade_in_ms = s_fade;
    tracks.clear();
    int n = nsf.playlist_mode ? 1 : nsf.songs;
    for (int i = 0; i < n; ++i) {
        int s = nsf.playlist_mode ? saved : i;
        nsf.song = s;
        TrackEntry t;
        t.index = s;
        int e = nsf.nsfe_plst ? nsf.nsfe_plst[s] : s;
        const char *lbl = (e >= 0 && e < (int)xgm::NSFE_ENTRIES) ? nsf.nsfe_entry[e].tlbl : nullptr;
        t.label = lbl ? lbl : "";
        if (nsf.playlist_mode && t.label.empty() && nsf.title) t.label = nsf.title;
        t.length_ms = nsf.GetLength();
        tracks.push_back(t);
    }
    nsf.song = saved;
    nsf.time_in_ms = st; nsf.loop_in_ms = sl; nsf.fade_in_ms = sf;
}

void Engine::ResetLocked() {
    if (!nsf.playlist_mode) {
        nsf.time_in_ms = s_time;
        nsf.loop_in_ms = s_loop;
        nsf.fade_in_ms = s_fade;
        nsf.playtime_unknown = s_unknown;
    }
    ApplyDefaultsLocked();
    player.SetPlayFreq(rate);
    player.SetChannels(2);
    player.SetSong(cur_song);
    player.Reset();
    frames_rendered = 0;
    ended = false;
}

void Engine::PlayTrack(int song) {
    Lock g(m);
    if (!loaded) return;
    if (nsf.playlist_mode) song = nsf.song;
    cur_song = song;
    ResetLocked();
    paused = false;
    playing = true;
}

void Engine::Play() {
    Lock g(m);
    if (!loaded) return;
    if (playing) { paused = false; return; }
    PlayTrack(cur_song);
}

void Engine::Pause() {
    if (!playing) return;
    paused = !paused;
}

void Engine::Stop() {
    Lock g(m);
    playing = false;
    paused = false;
    frames_rendered = 0;
}

void Engine::Seek(int ms) {
    Lock g(m);
    if (!loaded) return;
    if (!playing) { ResetLocked(); playing = true; paused = true; }
    if (ms < 0) ms = 0;
    long long target = (long long)ms * rate / 1000;
    long long cur = frames_rendered;
    if (target < cur) { ResetLocked(); cur = 0; }
    long long n = target - cur;
    bool fast = cfg["FAST_SEEK"].GetInt() != 0;
    std::vector<INT16> buf(4096 * 2);
    while (n > 0) {
        UINT32 c = (UINT32)std::min<long long>(n, 4096);
        if (fast) player.Skip(c); else player.Render(buf.data(), c);
        n -= c;
        frames_rendered += c;
        if (player.IsStopped()) { ended = true; break; }
    }
    delay_frames = 0;
}

int Engine::PositionMs() {
    long long f = frames_rendered - delay_frames;
    if (f < 0) f = 0;
    return (int)(f * 1000 / rate);
}

int Engine::LengthMs() {
    Lock g(m);
    if (!loaded) return 0;
    return nsf.GetLength();
}

bool Engine::ConsumeEnded() { return ended.exchange(false); }

void Engine::ApplyConfig() {
    Lock g(m);
    if (loaded) cfg.Notify(-1); // Reset() applies the full config on load anyway
    if (cfg["RATE"].GetInt() != rate) reopen = true;
}

std::string Engine::AudioError() {
    std::lock_guard<std::mutex> g(errm);
    return audio_err;
}

void Engine::GetChannels(std::vector<ChannelState> &out) {
    Lock g(m);
    out.assign(xgm::NES_CHANNEL_MAX, ChannelState());
    if (!loaded || !playing) return;
    int t = PositionMs();
    for (int i = 0; i < xgm::NES_CHANNEL_MAX; ++i) {
        int trk = xgm::NSFPlayerConfig::channel_track[i];
        auto *ti = dynamic_cast<xgm::ITrackInfo *>(player.GetInfo(t, trk));
        if (!ti) continue;
        ChannelState &c = out[i];
        c.valid = true;
        c.volume = ti->GetVolume();
        c.max_volume = ti->GetMaxVolume();
        if (c.max_volume <= 0) c.max_volume = 1;
        c.key = ti->GetKeyStatus();
        c.freq = ti->GetFreqHz();
    }
}

// ---------------------------------------------------------------- audio thread

void Engine::Thread() {
    snd_pcm_t *pcm = nullptr;
    const int CHUNK = 512;
    std::vector<INT16> buf(CHUNK * 2);
    std::vector<INT16> silence(CHUNK * 2, 0);
    auto seterr = [this](const std::string &e) {
        std::lock_guard<std::mutex> g(errm);
        audio_err = e;
    };

    while (!quit) {
        if (reopen) {
            reopen = false;
            if (pcm) { snd_pcm_drop(pcm); snd_pcm_close(pcm); pcm = nullptr; }
            int want = cfg["RATE"].GetInt();
            if (want < 8000 || want > 192000) want = 48000;
            const char *dev = getenv("NSFPLAY_ALSA_DEVICE");
            if (!dev || !*dev) dev = "default";
            int e = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
            if (e < 0) { seterr(std::string("ALSA open failed: ") + snd_strerror(e)); pcm = nullptr; }
            else {
                e = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16, SND_PCM_ACCESS_RW_INTERLEAVED,
                                       2, want, 1, 80000);
                if (e < 0) {
                    seterr(std::string("ALSA configure failed: ") + snd_strerror(e));
                    snd_pcm_close(pcm); pcm = nullptr;
                } else {
                    seterr("");
                    Lock g(m);
                    if (rate != want) {
                        // Rate changed. The player's sound chips only exist once an NSF is
                        // loaded, so only touch the player then (crash fix: RATE != 48000 at startup).
                        int pos = PositionMs();
                        rate = want;
                        if (loaded) {
                            player.SetPlayFreq(rate);
                            if (playing) Seek(pos);
                        }
                    }
                }
            }
        }
        if (!pcm) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); reopen = true; continue; }

        const INT16 *src = silence.data();
        if (playing && !paused && !ended) {
            Lock g(m);
            if (playing && !paused) {
                player.Render(buf.data(), CHUNK);
                frames_rendered += CHUNK;
                src = buf.data();
                if (player.IsStopped()) ended = true;
            }
        } else if (!playing || paused) {
            // idle: avoid spinning, keep device fed lightly
            snd_pcm_sframes_t d = 0;
            if (snd_pcm_delay(pcm, &d) == 0 && d > CHUNK * 4) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
        }
        snd_pcm_sframes_t w = snd_pcm_writei(pcm, src, CHUNK);
        if (w < 0) {
            w = snd_pcm_recover(pcm, (int)w, 1);
            if (w < 0) { seterr(std::string("ALSA write failed: ") + snd_strerror((int)w)); reopen = true; }
        }
        snd_pcm_sframes_t d = 0;
        if (playing && !paused && snd_pcm_delay(pcm, &d) == 0 && d >= 0) delay_frames = d;
    }
    if (pcm) { snd_pcm_drop(pcm); snd_pcm_close(pcm); }
}

// ---------------------------------------------------------------- WAV export

static void put16(std::ofstream &f, uint16_t v) { char b[2] = {(char)(v & 255), (char)(v >> 8)}; f.write(b, 2); }
static void put32(std::ofstream &f, uint32_t v) { for (int i = 0; i < 4; ++i) { char c = (char)((v >> (8 * i)) & 255); f.write(&c, 1); } }

bool Engine::ExportWav(int song, const std::string &out, std::string &err,
                       const std::function<bool(double)> &progress) {
    if (!loaded) { err = "No file loaded."; return false; }
    // Use a private NSF/player copy so playback is unaffected.
    std::string path;
    { Lock g(m); path = filename; }
    auto n2 = std::make_unique<xgm::NSF>();
    auto p2 = std::make_unique<xgm::NSFPlayer>();
    if (!n2->LoadFile(path.c_str())) { err = n2->LoadError(); return false; }
    n2->SetDefaults(cfg["PLAY_TIME"].GetInt(), cfg["FADE_TIME"].GetInt(), cfg["LOOP_NUM"].GetInt());
    int r = cfg["RATE"].GetInt();
    if (r < 8000 || r > 192000) r = 48000;
    {
        Lock g(m);
        p2->SetConfig(&cfg);
        p2->Load(n2.get());
        p2->SetPlayFreq(r);
        p2->SetChannels(2);
        if (!n2->playlist_mode) p2->SetSong(song);
        p2->Reset();
    }
    long long total = (long long)n2->GetLength() * r / 1000;
    std::ofstream f(out, std::ios::binary);
    if (!f) { cfg.DetachObserver(p2.get()); err = "Cannot open output file."; return false; }
    f.write("RIFF", 4); put32(f, 0); f.write("WAVEfmt ", 8);
    put32(f, 16); put16(f, 1); put16(f, 2); put32(f, r); put32(f, r * 4); put16(f, 4); put16(f, 16);
    f.write("data", 4); put32(f, 0);
    std::vector<INT16> buf(4096 * 2);
    long long done = 0;
    bool ok = true;
    while (done < total) {
        UINT32 c = (UINT32)std::min<long long>(4096, total - done);
        {
            Lock g(m); // config is shared with the live player
            p2->Render(buf.data(), c);
        }
        for (UINT32 i = 0; i < c * 2; ++i) put16(f, (uint16_t)buf[i]);
        done += c;
        if (p2->IsStopped()) break;
        if (progress && !progress((double)done / total)) { ok = false; err = "Cancelled."; break; }
    }
    uint32_t data = (uint32_t)(done * 4);
    f.seekp(4); put32(f, data + 36);
    f.seekp(40); put32(f, data);
    f.close();
    { Lock g(m); cfg.DetachObserver(p2.get()); }
    if (!ok) std::remove(out.c_str());
    return ok;
}

// ---------------------------------------------------------------- settings

void Engine::LoadSettings(const std::string &ini) {
    std::ifstream f(ini);
    if (!f) return;
    std::string line;
    Lock g(m);
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#' || line[0] == '[' || line[0] == ';') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (cfg.HasValue(k)) cfg.GetValue(k) = v;
    }
}

void Engine::SaveSettings(const std::string &ini) {
    std::ofstream f(ini);
    if (!f) return;
    f << "[NSFPlay]\n";
    for (auto &k : cfg.Keys()) f << k << "=" << cfg.GetValue(k).GetStr() << "\n";
}
