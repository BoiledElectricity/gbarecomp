// tts.cpp — see tts.h.

#include "tts.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

#include "runtime_arm.h"          // g_cpu, g_runtime_fn_entry_hook
#include "runtime_bus_bridge.h"   // active_bus()
#include "gba_bus.h"

#if defined(__ANDROID__)
#include <SDL.h>          // SDL_AndroidGetJNIEnv / SDL_AndroidGetActivity
#include <jni.h>
#elif defined(__APPLE__) || defined(__linux__)
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace gbarecomp::tts {
namespace {

#include "tts_charmap.inc"   // kGbaCharToUtf8
#include "tts_voices.inc"    // kGfxIdVoice

// Text encoding control bytes. FD prefixes a runtime placeholder, FC an
// extended command whose operand length varies; both are structure, not glyphs.
constexpr std::uint8_t kNewline   = 0xFE;
constexpr std::uint8_t kScroll    = 0xFA;
constexpr std::uint8_t kParagraph = 0xFB;
constexpr std::uint8_t kExtended  = 0xFC;
constexpr std::uint8_t kPlaceholder = 0xFD;
constexpr std::uint8_t kEnd       = 0xFF;

std::vector<Hook> g_hooks;                 // sorted by pc

// Follow the game's own typing. ShowFieldMessage hands over a whole multi-page
// string at once, so speaking it immediately runs the voice ahead of the text.
// The printer keeps a live pointer to the character it is drawing; tracking
// which page that pointer is in lets each page be spoken as it starts.
struct Page { std::size_t off; std::string text; };
std::uint32_t g_printers_addr = 0;         // sTextPrinters
std::uint32_t g_objevents_addr = 0;        // gObjectEvents
std::uint32_t g_selected_addr = 0;         // gSelectedObjectEvent
int g_voice_kind = 0;                      // 0 neutral, 1 male, 2 female

constexpr std::size_t kObjEventStride = 36;   // sizeof(struct ObjectEvent)
constexpr std::size_t kObjEventGfxOff = 5;    // graphicsId

const std::uint8_t* guest_ptr(gba::GbaBus* bus, std::uint32_t addr,
                              std::size_t* avail);   // fwd

// Who is talking: the object event the player interacted with, identified by
// the sprite it wears. A lass gets a lighter voice than a fat man.
int speaker_voice(gba::GbaBus* bus) {
    if (!g_objevents_addr || !g_selected_addr) return 0;
    std::size_t avail = 0;
    const std::uint8_t* sel = guest_ptr(bus, g_selected_addr, &avail);
    if (!sel || avail < 1) return 0;
    const unsigned idx = *sel;
    if (idx >= 16) return 0;
    const std::uint8_t* obj = guest_ptr(
        bus, g_objevents_addr + idx * kObjEventStride + kObjEventGfxOff, &avail);
    if (!obj || avail < 1) return 0;
    return kGfxIdVoice[*obj];
}
std::uint32_t g_str_base = 0;              // guest string being printed
std::size_t   g_str_len = 0;
std::vector<Page> g_pages;
int           g_spoken_page = -1;

constexpr std::size_t kPrinterStride = 36;   // sizeof(struct TextPrinter)
constexpr int         kPrinterCount  = 32;   // one per window
void (*g_prev_hook)(std::uint32_t) = nullptr;
bool g_armed = false;
std::string g_last;                        // suppress immediate repeats

bool env_off() {
    const char* e = std::getenv("GBARECOMP_TTS");
    return e && e[0] == '0';
}

// Side-effect-free guest read: the speaker must not perturb the machine it is
// listening to (bus.read* models open bus and prefetch).
const std::uint8_t* guest_ptr(gba::GbaBus* bus, std::uint32_t addr,
                              std::size_t* avail) {
    if (!bus) return nullptr;
    if (addr >= 0x02000000u && addr < 0x02040000u) {
        *avail = 0x40000u - (addr - 0x02000000u);
        return bus->ewram_ptr() + (addr - 0x02000000u);
    }
    if (addr >= 0x03000000u && addr < 0x03008000u) {
        *avail = 0x8000u - (addr - 0x03000000u);
        return bus->iwram_ptr() + (addr - 0x03000000u);
    }
    if (addr >= 0x08000000u && addr < 0x0A000000u) {
        const std::uint32_t off = addr - 0x08000000u;
        const std::uint8_t* rom = bus->rom_ptr();
        if (!rom || off >= bus->rom_size()) return nullptr;
        *avail = bus->rom_size() - off;
        return rom + off;
    }
    return nullptr;
}

#if defined(__ANDROID__)

// Speech goes through the Activity's TextToSpeech. SDL hands us a JNIEnv that
// is already attached for whichever thread calls, which matters because the
// hook runs on the game thread, not the Java one.
void android_call(const char* method, const std::string* arg) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) return;
    jclass cls = env->GetObjectClass(activity);
    jmethodID mid = env->GetMethodID(
        cls, method, arg ? "(Ljava/lang/String;)V" : "()V");
    if (mid) {
        if (arg) {
            jstring js = env->NewStringUTF(arg->c_str());
            env->CallVoidMethod(activity, mid, js);
            env->DeleteLocalRef(js);
        } else {
            env->CallVoidMethod(activity, mid);
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
}

void stop_voice() { android_call("stopSpeaking", nullptr); }

// Pitch rather than separate voices: every device has it, and it reads
// clearly as "a different person" without shipping voice packs.
void android_call_pitch(int kind) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) return;
    jclass cls = env->GetObjectClass(activity);
    jmethodID mid = env->GetMethodID(cls, "setVoiceKind", "(I)V");
    if (mid) env->CallVoidMethod(activity, mid, (jint)kind);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
}
void say(const std::string& text) {
    android_call_pitch(g_voice_kind);
    android_call("speakText", &text);
}

#elif defined(__APPLE__) || defined(__linux__)
pid_t g_voice = 0;

void stop_voice() {
    if (!g_voice) return;
    kill(g_voice, SIGTERM);
    int status = 0;
    waitpid(g_voice, &status, 0);
    g_voice = 0;
}

void say(const std::string& text) {
    stop_voice();               // interrupt, do not queue
#if defined(__APPLE__)
    const char* voice = g_voice_kind == 2 ? "Samantha"
                      : g_voice_kind == 1 ? "Daniel" : "Alex";
    const char* argv[] = {"say", "-v", voice, text.c_str(), nullptr};
    const char* bin = "/usr/bin/say";
#else
    const char* argv[] = {"spd-say", "-w", text.c_str(), nullptr};
    const char* bin = "/usr/bin/spd-say";
#endif
    pid_t pid = 0;
    if (posix_spawn(&pid, bin, nullptr, nullptr,
                    const_cast<char* const*>(argv), environ) == 0) {
        g_voice = pid;
    }
}
#else
void stop_voice() {}
void say(const std::string&) {}
#endif

unsigned long long g_entries = 0;
// Diagnostic: which guest functions actually get entered. Without this, "the
// hook is live but never sees our PC" has no next step.
std::set<std::uint32_t> g_seen;
bool g_trace_all = false;

void on_fn_entry(std::uint32_t entry_pc) {
    ++g_entries;
    const std::uint32_t pc = entry_pc & ~1u;
    if (g_trace_all) g_seen.insert(pc);
    auto it = std::lower_bound(g_hooks.begin(), g_hooks.end(), pc,
                               [](const Hook& h, std::uint32_t v) { return h.pc < v; });
    if (it != g_hooks.end() && it->pc == pc && !env_off()) {
        gba::GbaBus* bus = active_bus();
        std::size_t avail = 0;
        if (std::getenv("GBARECOMP_TTS_LOG")) {
            std::fprintf(stderr, "[tts] hit pc=%08X r0=%08X bus=%s\n",
                         pc, g_cpu.R[it->reg], bus ? "yes" : "NULL");
            std::fflush(stderr);
        }
        // R0 is the string, by the ABI the guest calls these with.
        if (const std::uint8_t* p = guest_ptr(bus, g_cpu.R[it->reg], &avail)) {
            const std::size_t cap = std::min<std::size_t>(avail, 1024);
            std::string text = decode(p, cap);
            if (!text.empty() && text != g_last) {
                g_last = text;
                // Split on the codes that make the box wait for the player:
                // those are exactly the points the voice should pause at.
                g_pages.clear();
                g_voice_kind = speaker_voice(bus);
                g_str_base = g_cpu.R[it->reg];
                g_spoken_page = -1;
                std::size_t start = 0;
                for (std::size_t i = 0; i < cap; ++i) {
                    const std::uint8_t b = p[i];
                    if (b == kEnd || b == kParagraph || b == kScroll) {
                        std::string page = decode(p + start, i - start);
                        if (!page.empty()) g_pages.push_back({start, page});
                        start = i + 1;
                        if (b == kEnd) { g_str_len = i + 1; break; }
                    }
                }
                if (g_pages.empty()) g_pages.push_back({0, text});
                if (std::getenv("GBARECOMP_TTS_LOG")) {
                    static const char* kind[] = {"neutral", "male", "female"};
                    std::fprintf(stderr, "[tts] (%s) \"%s\"\n",
                                 kind[g_voice_kind & 3], text.c_str());
                    std::fflush(stderr);
                }
                if (g_printers_addr == 0) {
                    say(text);          // no printer address: read it all now
                    g_pages.clear();
                } else {
                    say(g_pages[0].text);
                    g_spoken_page = 0;
                }
            }
        }
    }
    if (g_prev_hook) g_prev_hook(entry_pc);
}

}  // namespace

std::string decode(const std::uint8_t* bytes, std::size_t max) {
    std::string out;
    for (std::size_t i = 0; i < max; ++i) {
        const std::uint8_t b = bytes[i];
        if (b == kEnd) break;
        if (b == kNewline || b == kScroll || b == kParagraph) {
            // Line breaks are layout, not meaning. A space keeps the sentence
            // flowing instead of the voice running two words together.
            if (!out.empty() && out.back() != ' ') out.push_back(' ');
            continue;
        }
        if (b == kPlaceholder) { ++i; continue; }   // operand follows
        if (b == kExtended)    { ++i; continue; }   // best-effort skip
        if (const char* g = kGbaCharToUtf8[b]) out += g;
    }
    // Trim, and drop the trailing space a terminal line break leaves.
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

void init(const Config& cfg) {
    if (g_armed || cfg.hooks.empty()) return;
    g_hooks = cfg.hooks;
    for (auto& h : g_hooks) h.pc &= ~1u;
    std::sort(g_hooks.begin(), g_hooks.end(),
              [](const Hook& a, const Hook& b) { return a.pc < b.pc; });
    g_prev_hook = g_runtime_fn_entry_hook;
    g_runtime_fn_entry_hook = &on_fn_entry;
    g_armed = true;
    g_trace_all = std::getenv("GBARECOMP_TTS_TRACE") != nullptr;
    // atexit rather than the runtime's shutdown path: several exit routes skip
    // that, and a diagnostic that only fires on one of them is a trap.
    if (g_trace_all) std::atexit([] {
        if (FILE* f = std::fopen("/tmp/tts_seen_pcs.txt", "w")) {
            for (std::uint32_t pc : g_seen) std::fprintf(f, "0x%08X\n", pc);
            std::fclose(f);
        }
        std::fprintf(stderr, "[tts] %llu entries, %zu distinct PCs\n",
                     g_entries, g_seen.size());
        std::fflush(stderr);
    });
    std::fprintf(stderr, "[tts] speaking field text (%zu hook PC(s))\n",
                 g_hooks.size());
    for (const Hook& h : g_hooks)
        std::fprintf(stderr, "[tts]   watching 0x%08X (r%d)\n", h.pc, h.reg);
    std::fflush(stderr);
}

bool enabled() { return g_armed && !env_off(); }

void set_printers_addr(std::uint32_t addr) { g_printers_addr = addr; }

void set_speaker_addrs(std::uint32_t obj_events, std::uint32_t selected) {
    g_objevents_addr = obj_events;
    g_selected_addr = selected;
}

void tick() {
    if (!g_armed || env_off() || g_pages.empty() || !g_printers_addr) return;
    gba::GbaBus* bus = active_bus();
    std::size_t avail = 0;
    const std::uint8_t* base = guest_ptr(bus, g_printers_addr, &avail);
    if (!base || avail < kPrinterStride * kPrinterCount) return;

    // Which window the box uses is not fixed, so find whichever printer is
    // currently walking through the string we captured.
    for (int i = 0; i < kPrinterCount; ++i) {
        std::uint32_t cur = 0;
        std::memcpy(&cur, base + i * kPrinterStride, sizeof(cur));
        if (cur < g_str_base || cur >= g_str_base + g_str_len) continue;
        const std::size_t off = cur - g_str_base;
        int page = 0;
        for (int k = 0; k < static_cast<int>(g_pages.size()); ++k)
            if (off >= g_pages[k].off) page = k;
        if (page != g_spoken_page) {
            g_spoken_page = page;
            say(g_pages[page].text);
        }
        return;
    }
}

void speak(const std::string& utf8) { if (enabled()) say(utf8); }

void shutdown() {
    if (std::getenv("GBARECOMP_TTS_LOG")) {
        std::fprintf(stderr, "[tts] hook saw %llu function entries\n", g_entries);
        if (g_trace_all) {
            if (FILE* f = std::fopen("/tmp/tts_seen_pcs.txt", "w")) {
                for (std::uint32_t pc : g_seen) std::fprintf(f, "0x%08X\n", pc);
                std::fclose(f);
                std::fprintf(stderr, "[tts] %zu distinct PCs -> /tmp/tts_seen_pcs.txt\n",
                             g_seen.size());
            }
        }
        std::fflush(stderr);
    }
    stop_voice();
}

}  // namespace gbarecomp::tts
