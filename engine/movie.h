// Movie playback for ClientDE's video functions (StartVideo, UpdateVideo,
// StopVideo, IsVideoPlaying): a Smacker file shown full screen, frames paced
// by the game clock, its soundtrack fed to the mixer.
#ifndef SHOGO_ENGINE_MOVIE_H
#define SHOGO_ENGINE_MOVIE_H

#include "engine/smacker.h"
#include "engine/surfaces.h"

#include <memory>
#include <string>

namespace shogo {

class MoviePlayer {
public:
    // gamePath is the game's own (".\\Intro.smk"); false if it can't be played.
    bool start(const char *gamePath, std::string *err = nullptr);
    void stop();
    bool playing() const { return m_video != nullptr; }

    // Shows the frame due at `now` (game seconds), scaled to fit the screen;
    // the first call starts the clock. Stops once the last frame has had its time.
    void update(double now, Surface &screen);

private:
    void draw(Surface &screen) const;

    std::unique_ptr<SmackerVideo> m_video;
    double m_start = -1;
};

// Opens a game path: backslashes become slashes, and if the name doesn't exist
// as written, each part is matched without regard to case.
std::string resolveGamePath(const char *gamePath);

}  // namespace shogo

#endif
