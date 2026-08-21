// tts.h — speak the game's field text through the host's voice.
//
// For players who cannot read yet. The recompiled game is a native binary, so
// it can hand the text it was already about to draw to the platform's speech
// synthesiser; nothing in the ROM changes and the guest never knows.
//
// Scope is deliberately narrow: the field message path — people, signs, and
// the popups that use it. Battle text goes through a different guest function
// and is left alone, because narrating every attack would be noise.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gbarecomp::tts {

struct Hook {
    std::uint32_t pc = 0;      // guest function entry
    int           reg = 0;     // which argument register holds the string
};

struct Config {
    // Guest functions that display text worth speaking. Which register carries
    // the string depends on the function's signature — ShowFieldMessage takes
    // it first, AddTextPrinterParameterized2 takes it third — so the register
    // travels with the address rather than being assumed.
    std::vector<Hook> hooks;
};

// Arms the speaker and chains onto any existing function-entry hook.
// No-op when the config carries no addresses.
void init(const Config& cfg);

// False unless armed and GBARECOMP_TTS is not set to 0.
bool enabled();

// Decode a guest string (Gen 3 text encoding) to UTF-8. Stops at the
// terminator, at `max` bytes, or at an unmapped byte run.
std::string decode(const std::uint8_t* bytes, std::size_t max);

// Speak, replacing anything still being spoken — the player advancing a box
// should cut the previous line off rather than queue behind it.
void speak(const std::string& utf8);

// Address of the game's text-printer array (sTextPrinters). With it, speech
// follows the page the box is actually drawing; without it, a whole message is
// spoken at once.
void set_printers_addr(std::uint32_t addr);

// Addresses of the object-event array and the index of the one being talked
// to. With them, the speaker's sprite chooses the voice.
void set_speaker_addrs(std::uint32_t obj_events, std::uint32_t selected);

// Call once per frame: advances speech to the page now being printed.
void tick();

void shutdown();

}  // namespace gbarecomp::tts
